from __future__ import annotations

import asyncio
import os
import re
import signal
import socket
import subprocess
import threading
import time
from pathlib import Path

import raknet
from nethernet import serve_http
from nethernet.identity import IdentitySigner, generate_operator_key

REPOSITORY = Path(__file__).resolve().parents[2]
DEFAULT_BINARY = REPOSITORY / "build" / "RelWithDebInfo" / "endgate"
DEFAULT_ECHO = REPOSITORY / "build" / "RelWithDebInfo" / "tests" / "interop" / "endgate_nethernet_echo"


def free_port() -> int:
    """A loopback port free for UDP and TCP both, since endgate binds the two at one number."""
    while True:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            udp.bind(("127.0.0.1", 0))
            port = udp.getsockname()[1]
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as tcp:
                try:
                    tcp.bind(("127.0.0.1", port))
                except OSError:
                    continue
            return port


def normalize_log(line: str) -> str:
    line = re.sub(r"^\[\d\d:\d\d:\d\d (\w+)\]: ", r"\1 ", line)
    line = re.sub(r"(\d+\.\d+\.\d+\.\d+)[|:](\d+)", r"\1:\2", line)
    return line


class Proxy:
    """An endgate process listening on loopback, configured to send every player to one backend."""

    def __init__(
        self,
        binary: Path,
        workdir: Path,
        backend_port: int | None,
        max_players: int = 10,
        backend: str | None = None,
        debug: bool = False,
        tls: tuple[Path, Path] | None = None,
    ) -> None:
        self.binary = binary
        self.workdir = workdir
        self.port = free_port()
        self.backend_port = backend_port
        self.backend = backend or f"127.0.0.1:{backend_port}"
        self.max_players = max_players
        self.debug = debug
        self.tls = tls
        self.lines: list[str] = []
        self._listening = threading.Event()
        self.process: subprocess.Popen | None = None

    @property
    def address(self) -> tuple[str, int]:
        return "127.0.0.1", self.port

    @property
    def url(self) -> str:
        return f"http://127.0.0.1:{self.port}"

    def start(self) -> Proxy:
        for _ in range(5):
            if self._launch():
                return self
            self.port = free_port()
        raise RuntimeError("endgate did not start listening:\n" + "\n".join(self.lines))

    def _launch(self) -> bool:
        self.workdir.mkdir(parents=True, exist_ok=True)
        (self.workdir / "endgate.toml").write_text(
            "\n".join(
                [
                    'config-version = "1.0"',
                    f'bind = "127.0.0.1:{self.port}"',
                    'motd = "Endgate Test"',
                    f"show-max-players = {self.max_players}",
                    "online-mode = false",
                    f'tls-certificate = "{self.tls[0] if self.tls else ""}"',
                    f'tls-certificate-key = "{self.tls[1] if self.tls else ""}"',
                    "[servers]",
                    f'lobby = "{self.backend}"',
                    'try = ["lobby"]',
                    "[advanced]",
                    "compression-level = -1",
                    "",
                ]
            )
        )
        environment = dict(os.environ, SPDLOG_LEVEL="debug") if self.debug else None
        self.lines = []
        self._listening.clear()
        self.process = subprocess.Popen(
            [str(self.binary)],
            cwd=self.workdir,
            env=environment,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        threading.Thread(target=self._pump, args=(self.process,), daemon=True).start()
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline and self.process.poll() is None:
            if self._listening.wait(0.05):
                return True
        if self.process.poll() is None:
            self.stop()
            raise RuntimeError("endgate did not start listening:\n" + "\n".join(self.lines))
        return False

    def _pump(self, process: subprocess.Popen) -> None:
        assert process.stdout is not None
        for line in process.stdout:
            line = normalize_log(line.rstrip("\n"))
            self.lines.append(line)
            if line.startswith("INFO Listening on"):
                self._listening.set()

    def events(self, pattern: str = "") -> list[str]:
        startup = ("INFO Booting", "INFO Connections will use", "INFO Listening", "INFO Done")
        lines = [
            line.replace(f"127.0.0.1:{self.backend_port}", "<backend>").replace(f"127.0.0.1:{self.port}", "<proxy>")
            for line in self.lines
            if not line.startswith(startup)
        ]
        return [line for line in lines if re.search(pattern, line)]

    def wait_for(self, pattern: str, timeout: float = 5.0) -> bool:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.events(pattern):
                return True
            time.sleep(0.01)
        return False

    def alive(self) -> bool:
        return self.process is not None and self.process.poll() is None

    def stop(self) -> int | None:
        if self.process is None:
            return None
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
            try:
                self.process.wait(10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        return self.process.returncode


class EchoBackend:
    """A RakNet backend that sends every message straight back to the connection it came from."""

    def __init__(self) -> None:
        self.server = raknet.create_server(("127.0.0.1", free_port()), max_connections=32)
        self.port = self.server.local_address[1]
        threading.Thread(target=self.server.serve_forever, args=(self._echo,), daemon=True).start()

    @staticmethod
    def _echo(connection) -> None:
        for message in connection:
            connection.send(message)

    def close(self) -> None:
        self.server.close(0)


class Echo:
    """endgate's NetherNet stack behind one port, echoing every packet it receives."""

    def __init__(self, binary: Path) -> None:
        self.process = subprocess.Popen(
            [str(binary), "127.0.0.1"], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, text=True
        )
        first = self.process.stdout.readline().split()
        if first[:1] != ["listening"]:
            self.process.kill()
            raise RuntimeError(f"echo did not start: {first}")
        self.port = int(first[1])
        self.lines: list[str] = []
        threading.Thread(target=self._pump, daemon=True).start()

    @property
    def url(self) -> str:
        return f"http://127.0.0.1:{self.port}"

    def _pump(self) -> None:
        for line in self.process.stdout:
            self.lines.append(line.rstrip())

    def stop(self) -> None:
        self.process.terminate()
        self.process.wait(5)


class PythonServer:
    """nethernet-python's serve_http, an independent aiortc NetherNet server, running in its own loop."""

    def __init__(self) -> None:
        self._loop = asyncio.new_event_loop()
        self._ready = threading.Event()
        self._server = None
        self.port = 0
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()
        if not self._ready.wait(20):
            raise RuntimeError("python server did not start")

    def _run(self) -> None:
        asyncio.set_event_loop(self._loop)
        self._loop.run_until_complete(self._start())
        self._loop.run_forever()

    async def _start(self) -> None:
        signer = IdentitySigner(generate_operator_key(), domain="endgate-test")

        async def handler(connection) -> None:
            try:
                while True:
                    await connection.send(await connection.recv())
            except Exception:
                pass

        self._server = serve_http(handler, host="127.0.0.1", port=0, identity_signer=signer)
        await self._server.start()
        self.port = self._server.bound_port
        self._ready.set()

    def stop(self) -> None:
        async def close() -> None:
            await self._server.aclose()

        asyncio.run_coroutine_threadsafe(close(), self._loop).result(10)
        self._loop.call_soon_threadsafe(self._loop.stop)
        self._thread.join(10)
