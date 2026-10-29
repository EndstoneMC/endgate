import asyncio

import nethernet
import raknet

BATCH = bytes([0xFE])
SIZES = (1, 1400, 65535, 300000, 1_000_000)


def payload(size: int, seed: int) -> bytes:
    return bytes((i * 31 + seed) & 0xFF for i in range(size))


def test_a_raknet_player_reaches_a_nethernet_backend(make_proxy, nethernet_backend):
    proxy = make_proxy(nethernet_backend.port, debug=True)
    player = raknet.create_connection(proxy.address, timeout=5)
    try:
        for size in SIZES:
            message = BATCH + payload(size, size & 0xFF)
            player.send(message)
            assert player.recv(timeout=20) == message, size
    finally:
        player.close()
    assert proxy.events("DEBUG Server lobby uses NetherNet")
    assert proxy.wait_for(r"INFO \[server connection\] .* -> lobby has connected")


def test_a_nethernet_player_reaches_a_nethernet_backend(make_proxy, nethernet_backend):
    proxy = make_proxy(nethernet_backend.port)

    async def body():
        player = await nethernet.connect_http(proxy.url, timeout=20)
        for size in SIZES:
            data = payload(size, size & 0xFF)
            await player.send(data)
            assert await asyncio.wait_for(player.recv(), 20) == data, size
        await player.close()

    asyncio.run(asyncio.wait_for(body(), 120))
    assert proxy.wait_for(r"INFO \[server connection\] .* -> lobby has connected")


def test_a_burst_to_a_nethernet_backend_keeps_its_order(make_proxy, nethernet_backend):
    proxy = make_proxy(nethernet_backend.port)
    player = raknet.create_connection(proxy.address, timeout=5)
    try:
        assert proxy.wait_for(r"INFO \[server connection\] .* -> lobby has connected", 20), proxy.lines[-20:]
        sent = [BATCH + payload(1024, i) + i.to_bytes(4, "big") for i in range(500)]
        for message in sent:
            player.send(message)
        assert [player.recv(timeout=20) for _ in sent] == sent
    finally:
        player.close()


def test_closing_the_player_closes_the_nethernet_backend_leg(make_proxy, nethernet_backend):
    proxy = make_proxy(nethernet_backend.port)
    player = raknet.create_connection(proxy.address, timeout=5)
    player.send(BATCH + b"\x01hello")
    assert player.recv(timeout=20) == BATCH + b"\x01hello"
    player.close()
    assert proxy.wait_for(r"INFO \[server connection\] .* -> lobby has disconnected", 10), proxy.lines[-20:]
