import queue
import time

import pytest
from bedrock import Backend
from harness import free_port


@pytest.fixture
def round_trip(connect):
    def trip(proxy, backend: Backend | None) -> str | None:
        """Joins, says ping and returns what the backend heard, or None once the proxy has closed the player."""
        player = connect(proxy)
        assert player.wait_login()
        player.text("ping")
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline and not player.closed.is_set():
            if backend is None:
                player.wait_closed(0.1)
                continue
            try:
                return backend.accept(0.1).wait_text(10)
            except queue.Empty:
                continue
        return None

    return trip


def test_a_bare_address_on_a_raknet_server_is_detected_as_raknet(make_proxy, backend, round_trip):
    proxy = make_proxy(backend.port, debug=True)
    assert round_trip(proxy, backend) == "ping"
    assert proxy.events("DEBUG Server lobby uses RakNet")


def test_the_detected_transport_is_kept_for_the_next_player(make_proxy, backend, round_trip):
    proxy = make_proxy(backend.port, debug=True)
    assert round_trip(proxy, backend) == "ping"
    assert round_trip(proxy, backend) == "ping"
    assert len(proxy.events("DEBUG Server lobby uses")) == 1


def test_a_raknet_pin_skips_detection(make_proxy, backend, round_trip):
    proxy = make_proxy(backend.port, backend=f"raknet://127.0.0.1:{backend.port}", debug=True)
    assert round_trip(proxy, backend) == "ping"
    assert not proxy.events("DEBUG Server .* uses")


def test_a_backends_protocol_is_read_from_its_announcement(make_proxy, backend, round_trip):
    proxy = make_proxy(backend.port, debug=True)
    assert round_trip(proxy, backend) == "ping"
    assert proxy.events(r"DEBUG Server lobby speaks protocol 2225")


def test_a_backend_that_announces_no_protocol_is_still_reached(make_proxy, round_trip):
    quiet = Backend("raknet", protocol=0)
    try:
        proxy = make_proxy(quiet.port, debug=True)
        assert round_trip(proxy, quiet) == "ping"
        assert proxy.events(r"DEBUG Server lobby uses RakNet")
        assert not proxy.events(r"DEBUG Server .* speaks protocol")
    finally:
        quiet.close()


def test_a_bare_address_on_a_nethernet_server_is_detected_as_nethernet(make_proxy, nethernet_backend, round_trip):
    proxy = make_proxy(nethernet_backend.port, debug=True)
    assert round_trip(proxy, nethernet_backend) == "ping"
    assert proxy.events("DEBUG Server lobby uses NetherNet")


def test_a_nethernet_backend_that_gives_no_protocol_is_warned_about_once(make_proxy, nethernet_backend, round_trip):
    proxy = make_proxy(nethernet_backend.port)
    assert round_trip(proxy, nethernet_backend) == "ping"
    assert round_trip(proxy, nethernet_backend) == "ping"
    assert len(proxy.events(r"WARN Server lobby doesn't say which Minecraft version it runs")) == 1


def test_a_raknet_backend_that_gives_no_protocol_is_warned_about_once(make_proxy, round_trip):
    quiet = Backend("raknet", protocol=0)
    try:
        proxy = make_proxy(quiet.port)
        assert round_trip(proxy, quiet) == "ping"
        assert round_trip(proxy, quiet) == "ping"
        assert len(proxy.events(r"WARN Server lobby doesn't say which Minecraft version it runs")) == 1
    finally:
        quiet.close()


def test_a_raknet_backend_that_announces_a_protocol_is_not_warned_about(make_proxy, backend, round_trip):
    proxy = make_proxy(backend.port)
    assert round_trip(proxy, backend) == "ping"
    assert not proxy.events(r"WARN Server lobby doesn't say")


def test_a_nethernet_pin_skips_detection(make_proxy, nethernet_backend, round_trip):
    backend = f"nethernet://127.0.0.1:{nethernet_backend.port}"
    proxy = make_proxy(nethernet_backend.port, backend=backend, debug=True)
    assert round_trip(proxy, nethernet_backend) == "ping"
    assert not proxy.events("DEBUG Server .* uses")


def test_a_raknet_pin_on_a_nethernet_server_is_not_overridden(make_proxy, nethernet_backend, round_trip):
    backend = f"raknet://127.0.0.1:{nethernet_backend.port}"
    proxy = make_proxy(nethernet_backend.port, backend=backend, debug=True)
    assert round_trip(proxy, nethernet_backend) is None
    assert not proxy.events("DEBUG Server .* uses")


def test_a_backend_that_answers_nothing_closes_the_player(make_proxy, round_trip):
    proxy = make_proxy(free_port())
    started = time.monotonic()
    assert round_trip(proxy, None) is None
    assert time.monotonic() - started < 5
    assert proxy.events(r"ERROR \[connected player\] .*: unable to connect to server lobby")


def test_a_failed_connection_detects_again(make_proxy, round_trip):
    server = Backend("raknet")
    proxy = make_proxy(server.port, debug=True)
    try:
        assert round_trip(proxy, server) == "ping"
    finally:
        server.close()
    assert round_trip(proxy, None) is None
    assert round_trip(proxy, None) is None
    assert len(proxy.events("DEBUG Server lobby uses RakNet")) == 1
    assert len(proxy.events("DEBUG Server lobby supports neither NetherNet nor RakNet")) == 1
