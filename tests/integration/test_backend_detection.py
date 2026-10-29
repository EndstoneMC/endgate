import time

import raknet
from harness import EchoBackend, free_port

BATCH = bytes([0xFE])


def round_trip(proxy) -> bytes | None:
    """Connects a RakNet player, sends one batch and returns the echo, or None once the proxy closes it."""
    player = raknet.create_connection(proxy.address, timeout=5)
    try:
        player.send(BATCH + b"\x01ping")
        return player.recv(timeout=15)
    except raknet.ConnectionClosed:
        return None
    finally:
        player.close()


def test_a_bare_address_on_a_raknet_server_is_detected_as_raknet(make_proxy, echo_backend):
    proxy = make_proxy(echo_backend.port, debug=True)
    assert round_trip(proxy) == BATCH + b"\x01ping"
    assert proxy.events("DEBUG Backend <backend> speaks RakNet")


def test_the_detected_transport_is_kept_for_the_next_player(make_proxy, echo_backend):
    proxy = make_proxy(echo_backend.port, debug=True)
    assert round_trip(proxy) == BATCH + b"\x01ping"
    assert round_trip(proxy) == BATCH + b"\x01ping"
    assert len(proxy.events("DEBUG Backend <backend> speaks")) == 1


def test_a_raknet_pin_skips_detection(make_proxy, echo_backend):
    proxy = make_proxy(echo_backend.port, backend=f"raknet://127.0.0.1:{echo_backend.port}", debug=True)
    assert round_trip(proxy) == BATCH + b"\x01ping"
    assert not proxy.events("DEBUG Backend .* speaks")


def test_a_bare_address_on_a_nethernet_server_is_detected_as_nethernet(make_proxy, nethernet_backend):
    proxy = make_proxy(nethernet_backend.port, debug=True)
    assert round_trip(proxy) == BATCH + b"\x01ping"
    assert proxy.events("DEBUG Backend <backend> speaks NetherNet")


def test_a_nethernet_pin_skips_detection(make_proxy, nethernet_backend):
    backend = f"nethernet://127.0.0.1:{nethernet_backend.port}"
    proxy = make_proxy(nethernet_backend.port, backend=backend, debug=True)
    assert round_trip(proxy) == BATCH + b"\x01ping"
    assert not proxy.events("DEBUG Backend .* speaks")


def test_a_raknet_pin_on_a_nethernet_server_is_not_overridden(make_proxy, nethernet_backend):
    backend = f"raknet://127.0.0.1:{nethernet_backend.port}"
    proxy = make_proxy(nethernet_backend.port, backend=backend, debug=True)
    assert round_trip(proxy) is None
    assert not proxy.events("DEBUG Backend .* speaks")


def test_a_backend_that_answers_nothing_closes_the_player(make_proxy):
    port = free_port()
    proxy = make_proxy(port)
    started = time.monotonic()
    assert round_trip(proxy) is None
    assert time.monotonic() - started < 5
    assert proxy.events("WARN Can't reach backend <backend> for")


def test_a_failed_connection_detects_again(make_proxy):
    echo = EchoBackend()
    proxy = make_proxy(echo.port, debug=True)
    try:
        assert round_trip(proxy) == BATCH + b"\x01ping"
    finally:
        echo.close()
    assert round_trip(proxy) is None
    assert round_trip(proxy) is None
    assert len(proxy.events("DEBUG Backend <backend> speaks RakNet")) == 1
    assert len(proxy.events("DEBUG Backend <backend> answers neither NetherNet nor RakNet")) == 1
