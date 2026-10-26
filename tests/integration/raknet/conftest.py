from __future__ import annotations

import json
import os
import re
import signal
import socket
import subprocess
import threading
import time
from pathlib import Path

import pytest
import raknet

REPOSITORY = Path(__file__).resolve().parents[3]
DEFAULT_BINARY = REPOSITORY / "build" / "RelWithDebInfo" / "endgate"

_observations: dict[str, object] = {}


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def normalize_log(line: str) -> str:
    line = re.sub(r"^\[\d\d:\d\d:\d\d (\w+)\]: ", r"\1 ", line)
    line = re.sub(r"(\d+\.\d+\.\d+\.\d+)[|:](\d+)", r"\1:\2", line)
    return line


class Proxy:
    """An endgate process listening on loopback, configured to send every player to one backend."""

    def __init__(self, binary: Path, workdir: Path, backend_port: int, max_players: int = 10) -> None:
        self.binary = binary
        self.workdir = workdir
        self.port = free_port()
        self.backend_port = backend_port
        self.max_players = max_players
        self.lines: list[str] = []
        self._listening = threading.Event()
        self.process: subprocess.Popen | None = None

    @property
    def address(self) -> tuple[str, int]:
        return "127.0.0.1", self.port

    def start(self) -> Proxy:
        self.workdir.mkdir(parents=True, exist_ok=True)
        (self.workdir / "endgate.toml").write_text(
            "\n".join(
                [
                    'config-version = "1.0"',
                    f'bind = "127.0.0.1:{self.port}"',
                    'motd = "Endgate Test"',
                    f"show-max-players = {self.max_players}",
                    "online-mode = false",
                    "[servers]",
                    f'lobby = "127.0.0.1:{self.backend_port}"',
                    'try = ["lobby"]',
                    "[advanced]",
                    "compression-level = -1",
                    "",
                ]
            )
        )
        self.process = subprocess.Popen(
            [str(self.binary)],
            cwd=self.workdir,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        threading.Thread(target=self._pump, daemon=True).start()
        if not self._listening.wait(10):
            self.stop()
            raise RuntimeError("endgate did not start listening:\n" + "\n".join(self.lines))
        return self

    def _pump(self) -> None:
        assert self.process is not None and self.process.stdout is not None
        for line in self.process.stdout:
            line = normalize_log(line.rstrip("\n"))
            self.lines.append(line)
            if line.startswith("INFO Listening on"):
                self._listening.set()

    def events(self, pattern: str = "") -> list[str]:
        startup = ("INFO Booting", "INFO Connections will use", "INFO Listening", "INFO Done")
        lines = [line for line in self.lines if not line.startswith(startup) and re.search(pattern, line)]
        return [
            line.replace(f"127.0.0.1:{self.backend_port}", "<backend>").replace(f"127.0.0.1:{self.port}", "<proxy>")
            for line in lines
        ]

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


def pytest_addoption(parser: pytest.Parser) -> None:
    parser.addoption("--endgate", default=os.environ.get("ENDGATE_BINARY", str(DEFAULT_BINARY)))
    parser.addoption("--observations", default=os.environ.get("ENDGATE_OBSERVATIONS"))


@pytest.fixture(scope="session")
def binary(request: pytest.FixtureRequest) -> Path:
    path = Path(request.config.getoption("--endgate"))
    if not path.exists():
        pytest.exit(f"endgate binary not found at {path}")
    return path


@pytest.fixture
def observe(request: pytest.FixtureRequest):
    """Record a value the A/B runner compares between two builds."""

    def record(name: str, value: object) -> object:
        _observations[f"{request.node.nodeid}::{name}"] = json.loads(json.dumps(value, default=list))
        return value

    return record


def pytest_sessionfinish(session: pytest.Session) -> None:
    target = session.config.getoption("--observations")
    if target:
        Path(target).write_text(json.dumps(_observations, indent=1, sort_keys=True))


@pytest.fixture
def backend():
    server = raknet.create_server(("127.0.0.1", free_port()), max_connections=32)
    yield server
    server.close(0)


@pytest.fixture
def make_proxy(binary: Path, tmp_path: Path):
    started: list[Proxy] = []

    def make(backend_port: int, max_players: int = 10) -> Proxy:
        proxy = Proxy(binary, tmp_path / f"proxy{len(started)}", backend_port, max_players).start()
        started.append(proxy)
        return proxy

    yield make
    for proxy in started:
        assert proxy.alive(), "endgate exited during the test:\n" + "\n".join(proxy.lines[-40:])
        assert proxy.stop() == 0, "endgate did not shut down cleanly:\n" + "\n".join(proxy.lines[-40:])


@pytest.fixture
def proxy(make_proxy, backend) -> Proxy:
    return make_proxy(backend.local_address[1])
