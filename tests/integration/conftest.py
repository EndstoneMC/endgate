from __future__ import annotations

import json
import os
import sys
from pathlib import Path

import pytest
from bedrock import Backend, Player, Services, make_player
from harness import DEFAULT_BINARY, Proxy

_observations: dict[str, object] = {}


def pytest_addoption(parser: pytest.Parser) -> None:
    parser.addoption("--endgate", default=os.environ.get("ENDGATE_BINARY", str(DEFAULT_BINARY)))
    parser.addoption("--observations", default=os.environ.get("ENDGATE_OBSERVATIONS"))


@pytest.fixture(scope="session")
def binary(request: pytest.FixtureRequest) -> Path:
    path = Path(request.config.getoption("--endgate"))
    if not path.exists():
        pytest.exit(f"endgate binary not found at {path}")
    return path


@pytest.fixture(scope="session", autouse=True)
def drain_datagrams_on_close():
    if sys.platform != "win32":
        yield
        return
    from asyncio import proactor_events

    transport = proactor_events._ProactorDatagramTransport
    loop_writing = transport._loop_writing

    def drain(self, fut=None):
        if not self._closing:
            return loop_writing(self, fut)
        self._write_fut = None
        try:
            if fut is not None:
                fut.result()
            if self._buffer:
                data, addr = self._buffer.popleft()
                self._buffer_size -= len(data) + self._header_size
                self._write_fut = self._loop._proactor.sendto(self._sock, data, addr=addr)
                self._write_fut.add_done_callback(self._loop_writing)
                return
        except OSError:
            self._buffer.clear()
            self._buffer_size = 0
        self._loop.call_soon(self._call_connection_lost, None)

    transport._loop_writing = drain
    yield
    transport._loop_writing = loop_writing


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
def services():
    """A stand-in for Minecraft Services; a proxy given it runs in online mode."""
    server = Services()
    yield server
    server.close()


@pytest.fixture
def backend():
    server = Backend("raknet")
    yield server
    server.close()


@pytest.fixture
def encrypted_backend():
    server = Backend("raknet", encrypt=True)
    yield server
    server.close()


@pytest.fixture
def nethernet_backend():
    server = Backend("nethernet")
    yield server
    server.close()


@pytest.fixture
def connect(tmp_path: Path):
    """Joins a proxy as a Bedrock player; every player is closed when the test ends."""
    players: list[Player] = []

    def join(proxy, transport: str = "raknet", **options) -> Player:
        player = make_player(proxy, tmp_path, transport, **options)
        players.append(player)
        return player

    yield join
    for player in players:
        player.close()


@pytest.fixture
def make_proxy(binary: Path, tmp_path: Path):
    started: list[Proxy] = []

    def make(
        backend_port: int | None,
        max_players: int = 10,
        backend: str | None = None,
        debug: bool = False,
        tls: tuple[Path, Path] | None = None,
        log_player_connections: bool = True,
        packet_decode_logging: bool = False,
        services: Services | None = None,
        servers: dict[str, str] | None = None,
        try_order: list[str] | None = None,
        console: bool = False,
        settings: dict[str, dict[str, str]] | None = None,
    ) -> Proxy:
        workdir = tmp_path / f"proxy{len(started)}"
        proxy = Proxy(
            binary,
            workdir,
            backend_port,
            max_players,
            backend,
            debug,
            tls,
            log_player_connections,
            packet_decode_logging,
            services,
            servers=servers,
            try_order=try_order,
            console=console,
            settings=settings,
        ).start()
        started.append(proxy)
        return proxy

    yield make
    for proxy in started:
        assert proxy.alive(), "endgate exited during the test:\n" + "\n".join(proxy.lines[-40:])
        assert proxy.stop() == 0, "endgate did not shut down cleanly:\n" + "\n".join(proxy.lines[-40:])


@pytest.fixture
def proxy(make_proxy, backend) -> Proxy:
    return make_proxy(backend.port)
