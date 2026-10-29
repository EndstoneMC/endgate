from __future__ import annotations

import json
import os
from pathlib import Path

import pytest
import raknet
from harness import DEFAULT_BINARY, DEFAULT_ECHO, Echo, EchoBackend, Proxy, PythonServer, free_port

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
def echo_backend():
    server = EchoBackend()
    yield server
    server.close()


@pytest.fixture(params=["endgate", "aiortc"])
def nethernet_backend(request: pytest.FixtureRequest):
    """A NetherNet server that sends every batch back: endgate's own echo harness, or nethernet-python's."""
    if request.param == "aiortc":
        server = PythonServer()
        yield server
        server.stop()
        return
    binary = Path(os.environ.get("ENDGATE_NETHERNET_ECHO", DEFAULT_ECHO))
    if not binary.exists():
        pytest.exit(f"endgate_nethernet_echo not found at {binary}")
    server = Echo(binary)
    yield server
    server.stop()


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
        ).start()
        started.append(proxy)
        return proxy

    yield make
    for proxy in started:
        assert proxy.alive(), "endgate exited during the test:\n" + "\n".join(proxy.lines[-40:])
        assert proxy.stop() == 0, "endgate did not shut down cleanly:\n" + "\n".join(proxy.lines[-40:])


@pytest.fixture
def proxy(make_proxy, backend) -> Proxy:
    return make_proxy(backend.local_address[1])
