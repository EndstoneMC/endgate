import asyncio
import socket

import nethernet
import pytest
import raknet

BATCH = bytes([0xFE])


def batches(count: int) -> list[bytes]:
    return [b"\x01early" + i.to_bytes(2, "big") for i in range(count)]


def test_a_raknet_player_s_first_batch_waits_for_the_backend(make_proxy, echo_backend):
    proxy = make_proxy(echo_backend.port)
    player = raknet.create_connection(proxy.address, timeout=5)
    try:
        sent = [BATCH + batch for batch in batches(20)]
        for message in sent:
            player.send(message)
        assert [player.recv(timeout=10) for _ in sent] == sent
    finally:
        player.close()


def test_a_nethernet_player_s_first_batch_waits_for_the_backend(make_proxy, echo_backend):
    proxy = make_proxy(echo_backend.port)

    async def body():
        player = await nethernet.connect_http(proxy.url, timeout=20)
        sent = batches(20)
        for batch in sent:
            await player.send(batch)
        received = [await asyncio.wait_for(player.recv(), 10) for _ in sent]
        await player.close()
        return sent, received

    sent, received = asyncio.run(asyncio.wait_for(body(), 60))
    assert received == sent


def test_a_first_batch_waits_for_a_nethernet_backend(make_proxy, nethernet_backend):
    proxy = make_proxy(nethernet_backend.port)
    player = raknet.create_connection(proxy.address, timeout=5)
    try:
        sent = [BATCH + batch for batch in batches(20)]
        for message in sent:
            player.send(message)
        assert [player.recv(timeout=20) for _ in sent] == sent
    finally:
        player.close()


@pytest.fixture
def silent_backend():
    """A UDP port that takes datagrams and never answers, so a leg to it stays connecting."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sink:
        sink.bind(("127.0.0.1", 0))
        yield sink.getsockname()[1]


def test_too_much_before_the_backend_is_up_closes_the_player(make_proxy, silent_backend):
    proxy = make_proxy(silent_backend, backend=f"raknet://127.0.0.1:{silent_backend}")
    player = raknet.create_connection(proxy.address, timeout=5)
    try:
        player.send(BATCH + bytes(32 * 1024))
        player.send(BATCH + bytes(33 * 1024))
        with pytest.raises(raknet.ConnectionClosed):
            player.recv(timeout=5)
    finally:
        player.close()
    assert proxy.events(r"WARN \[server connection\] .* -> lobby: more than 65536 bytes arrived before the server was up")
