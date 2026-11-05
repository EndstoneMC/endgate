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

import aioice.ice
import nethernet
from aiortc.rtcicetransport import RTCIceGatherer
from nethernet import serve_http
from nethernet.identity import IdentitySigner, generate_operator_key

REPOSITORY = Path(__file__).resolve().parents[2]
# aiortc gathers a reflexive candidate from a public STUN server unless told otherwise, which costs
# about five seconds and outlasts the proxy's connect deadline. nethernet-python's ice_servers=[] is
# falsy, so it cannot say "none"; take the default away instead and keep gathering on the loopback.
RTCIceGatherer.getDefaultIceServers = staticmethod(list)
if os.name == "nt":
    aioice.ice.get_host_addresses = lambda use_ipv4, use_ipv6: ["127.0.0.1"] if use_ipv4 else []

EXE = ".exe" if os.name == "nt" else ""
DEFAULT_BINARY = REPOSITORY / "build" / "RelWithDebInfo" / f"endgate{EXE}"
DEFAULT_ECHO = REPOSITORY / "build" / "RelWithDebInfo" / "tests" / "interop" / f"endgate_nethernet_echo{EXE}"


PLAYER_IDENTITY = IdentitySigner(generate_operator_key(), domain="self")


async def connect_player(url: str, identity_signer=PLAYER_IDENTITY) -> nethernet.Connection:
    """A NetherNet player dialling as a Bedrock client does, its offer signed with its own key."""
    return await nethernet.connect_http(url, timeout=20, identity_signer=identity_signer)


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
        log_player_connections: bool = True,
        packet_decode_logging: bool = False,
        services=None,
        login_ratelimit: int = 0,
        servers: dict[str, str] | None = None,
        try_order: list[str] | None = None,
        console: bool = False,
        settings: dict[str, dict[str, str]] | None = None,
    ) -> None:
        """settings adds raw TOML values by section: "" for top-level keys, else the section's name."""
        self.binary = binary
        self.workdir = workdir
        self.port = free_port()
        self.backend_port = backend_port
        self.backend = backend or f"127.0.0.1:{backend_port}"
        self.max_players = max_players
        self.debug = debug
        self.tls = tls
        self.log_player_connections = log_player_connections
        self.packet_decode_logging = packet_decode_logging
        self.login_ratelimit = login_ratelimit
        self.services = services
        self.servers = servers or {}
        self.try_order = try_order or ["lobby"]
        self.console = console
        self.settings = settings or {}
        self.lines: list[str] = []
        self._listening = threading.Event()
        self.process: subprocess.Popen | None = None

    def _settings(self, section: str) -> list[str]:
        return [f"{key} = {value}" for key, value in self.settings.get(section, {}).items()]

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
                    f"online-mode = {str(self.services is not None).lower()}",
                    f'tls-certificate = "{self.tls[0].as_posix() if self.tls else ""}"',
                    f'tls-certificate-key = "{self.tls[1].as_posix() if self.tls else ""}"',
                    *(
                        []
                        if "player-info-forwarding-mode" in self.settings.get("", {})
                        else ['player-info-forwarding-mode = "none"']
                    ),
                    *self._settings(""),
                    "[servers]",
                    f'lobby = "{self.backend}"',
                    *(f'{name} = "{address}"' for name, address in self.servers.items()),
                    "try = [" + ", ".join(f'"{name}"' for name in self.try_order) + "]",
                    *self._settings("servers"),
                    "[advanced]",
                    "compression-level = -1",
                    f"login-ratelimit = {self.login_ratelimit}",
                    f"log-player-connections = {str(self.log_player_connections).lower()}",
                    *self._settings("advanced"),
                    "[debug]",
                    f"packet-decode-logging = {str(self.packet_decode_logging).lower()}",
                    *self._settings("debug"),
                    *(
                        []
                        if "metrics" in self.settings or "ENDGATE_BSTATS_URI" in os.environ
                        else ["[metrics]", "enabled = false"]
                    ),
                    *(
                        line
                        for section in self.settings
                        if section not in ("", "servers", "advanced", "debug")
                        for line in (f"[{section}]", *self._settings(section))
                    ),
                    "",
                ]
            )
        )
        bstats_config = self.workdir / "plugins" / "bstats" / "config.toml"
        if "ENDGATE_BSTATS_URI" not in os.environ and not bstats_config.exists():
            bstats_config.parent.mkdir(parents=True, exist_ok=True)
            bstats_config.write_text('enabled = false\nserver-uuid = "00000000-0000-0000-0000-000000000000"\n')
        environment = dict(os.environ)
        if self.debug:
            environment["SPDLOG_LEVEL"] = "debug"
        if self.services is not None:
            environment["ENDGATE_DISCOVERY_URI"] = self.services.discovery_uri
        self.lines = []
        self._listening.clear()
        self.process = subprocess.Popen(
            [str(self.binary)],
            cwd=self.workdir,
            env=environment,
            stdin=subprocess.PIPE if self.console or os.name == "nt" else subprocess.DEVNULL,
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
        startup = (
            "INFO Starting",
            "WARN Player info forwarding is disabled!",
            "INFO The forwarding-key-file does not exist.",
            "WARN The proxy is running in offline mode!",
            "WARN Minecraft services are discovered at",
            "INFO Connections will use",
            "INFO Listening",
            "INFO Done",
        )
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

    def command(self, line: str) -> None:
        """Types a line on the proxy's console."""
        assert self.process is not None and self.process.stdin is not None
        self.process.stdin.write(line + "\n")
        self.process.stdin.flush()

    def alive(self) -> bool:
        return self.process is not None and self.process.poll() is None

    def stop(self) -> int | None:
        if self.process is None:
            return None
        if os.name == "nt" and self.process.poll() is None and self.process.stdin is not None:
            try:
                self.command("shutdown")
            except OSError:
                pass
        if self.process.stdin is not None:
            self.process.stdin.close()
        if self.process.poll() is None:
            if os.name != "nt":
                self.process.send_signal(signal.SIGTERM)
            try:
                self.process.wait(10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        return self.process.returncode


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
