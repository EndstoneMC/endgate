import socket

import pytest
from bedrock import Backend, noise


@pytest.fixture
def slow_backend():
    """A RakNet backend that takes a while to finish each login, so the player is ahead of it."""
    server = Backend("raknet", delay=500)
    yield server
    server.close()


@pytest.fixture
def slow_nethernet_backend():
    server = Backend("nethernet", delay=500)
    yield server
    server.close()


def early(count: int) -> list[str]:
    return [f"early {i}" for i in range(count)]


@pytest.mark.parametrize("transport", ["raknet", "nethernet"])
def test_a_player_s_first_batches_wait_for_the_backend(make_proxy, slow_backend, connect, transport):
    proxy = make_proxy(slow_backend.port)
    player = connect(proxy, transport)
    assert player.wait_login()
    sent = early(20)
    for message in sent:
        player.text(message)
    upstream = slow_backend.accept(20)
    assert [upstream.wait_text(20) for _ in sent] == sent


def test_a_first_batch_waits_for_a_nethernet_backend(make_proxy, slow_nethernet_backend, connect):
    proxy = make_proxy(slow_nethernet_backend.port)
    player = connect(proxy)
    assert player.wait_login()
    sent = early(20)
    for message in sent:
        player.text(message)
    upstream = slow_nethernet_backend.accept(30)
    assert [upstream.wait_text(30) for _ in sent] == sent


@pytest.fixture
def silent_backend():
    """A UDP port that takes datagrams and never answers, so a leg to it stays connecting."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sink:
        sink.bind(("127.0.0.1", 0))
        yield sink.getsockname()[1]


def test_too_much_before_the_backend_is_up_closes_the_player(make_proxy, silent_backend, connect):
    proxy = make_proxy(silent_backend, backend=f"raknet://127.0.0.1:{silent_backend}")
    player = connect(proxy)
    assert player.wait_login()
    player.text(noise(32 * 1024, 1))
    player.text(noise(33 * 1024, 2))
    assert player.wait_closed(5)
    assert proxy.events(
        r"WARN \[server connection\] .* -> lobby: more than 65536 bytes arrived before the server was up"
    )
