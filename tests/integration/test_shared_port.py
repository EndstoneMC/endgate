import asyncio

import nethernet
import raknet

BATCH = bytes([0xFE])


def test_raknet_and_nethernet_players_share_the_port(make_proxy, echo_backend):
    proxy = make_proxy(echo_backend.port)

    def raknet_player(index: int) -> bytes:
        player = raknet.create_connection(proxy.address, timeout=5)
        try:
            player.send(BATCH + b"raknet" + bytes([index]))
            return player.recv(timeout=10)
        finally:
            player.close()

    async def nethernet_player(index: int) -> bytes:
        player = await nethernet.connect_http(proxy.url, timeout=20)
        await player.send(b"nethernet" + bytes([index]))
        reply = await asyncio.wait_for(player.recv(), 10)
        await player.close()
        return reply

    async def body():
        return await asyncio.gather(
            nethernet_player(0),
            nethernet_player(1),
            asyncio.to_thread(raknet_player, 0),
            asyncio.to_thread(raknet_player, 1),
        )

    replies = asyncio.run(asyncio.wait_for(body(), 60))
    assert replies == [b"nethernet\x00", b"nethernet\x01", BATCH + b"raknet\x00", BATCH + b"raknet\x01"]
    assert len(proxy.events(r"INFO \[initial connection\] .* has connected")) == 4
    assert len(proxy.events(r"INFO \[server connection\] .* -> lobby has connected")) == 4
