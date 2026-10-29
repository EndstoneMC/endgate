import asyncio
import re

import nethernet
import raknet
from nethernet import SendType

BATCH = bytes([0xFE])


def run(coroutine):
    return asyncio.run(asyncio.wait_for(coroutine, 120))


def payload(size: int, seed: int) -> bytes:
    return bytes((i * 31 + seed) & 0xFF for i in range(size))


async def join(proxy, backend):
    """A NetherNet player on the proxy, and the connection the proxy opened to the RakNet backend for it."""
    player = await nethernet.connect_http(proxy.url, timeout=20)
    upstream = await asyncio.to_thread(backend.accept, 10)
    assert await asyncio.to_thread(proxy.wait_for, "-> lobby has connected", 5), proxy.lines
    return player, upstream


async def receive(connection, timeout: float = 10) -> bytes:
    return await asyncio.to_thread(connection.recv, timeout)


def test_a_batch_reaches_the_backend_with_the_raknet_prefix(proxy, backend):
    async def body():
        player, upstream = await join(proxy, backend)
        await player.send(b"\x01hello")
        assert await receive(upstream) == BATCH + b"\x01hello"
        await player.close()

    run(body())


def test_a_backend_batch_reaches_the_player_without_the_prefix(proxy, backend):
    async def body():
        player, upstream = await join(proxy, backend)
        upstream.send(BATCH + b"\x02world")
        assert await asyncio.wait_for(player.recv(), 10) == b"\x02world"
        await player.close()

    run(body())


def test_a_backend_message_without_the_prefix_is_dropped(proxy, backend):
    async def body():
        player, upstream = await join(proxy, backend)
        upstream.send(b"\x03stray")
        upstream.send(BATCH + b"\x04next")
        assert await asyncio.wait_for(player.recv(), 10) == b"\x04next"
        await player.close()

    run(body())


def test_large_batches_cross_both_ways(proxy, backend):
    async def body():
        player, upstream = await join(proxy, backend)
        for size in (1, 1400, 65535, 262143, 262144, 300000, 1_000_000):
            data = payload(size, size & 0xFF)
            await player.send(data)
            assert await receive(upstream, 20) == BATCH + data, size
            reply = data[::-1]
            upstream.send(BATCH + reply)
            assert await asyncio.wait_for(player.recv(), 20) == reply, size
        await player.close()

    run(body())


def test_a_burst_keeps_its_order_both_ways(proxy, backend):
    async def body():
        player, upstream = await join(proxy, backend)
        sent = [payload(1024, i) + i.to_bytes(4, "big") for i in range(200)]
        for data in sent:
            await player.send(data)
        assert [await receive(upstream, 20) for _ in sent] == [BATCH + data for data in sent]
        for data in sent:
            upstream.send(BATCH + data[::-1])
        assert [await asyncio.wait_for(player.recv(), 20) for _ in sent] == [data[::-1] for data in sent]
        await player.close()

    run(body())


def test_an_unreliable_batch_is_forwarded(proxy, backend):
    async def body():
        player, upstream = await join(proxy, backend)
        await player.send(b"\x05movement", SendType.UNRELIABLE)
        assert await receive(upstream) == BATCH + b"\x05movement"
        await player.close()

    run(body())


def test_closing_the_player_closes_the_backend(proxy, backend):
    async def body():
        player, upstream = await join(proxy, backend)
        await player.close()
        try:
            await receive(upstream)
        except raknet.ConnectionClosed:
            return "closed"
        except TimeoutError:
            return "timeout"
        return "message"

    assert run(body()) == "closed"


def test_closing_the_backend_closes_the_player(proxy, backend):
    async def body():
        player, upstream = await join(proxy, backend)
        upstream.close()
        try:
            await asyncio.wait_for(player.recv(), 15)
        except nethernet.ConnectionClosed:
            return "closed"
        except TimeoutError:
            return "timeout"
        return "message"

    assert run(body()) == "closed"


def test_the_player_is_logged_like_a_raknet_player(proxy, backend):
    async def body():
        player, upstream = await join(proxy, backend)
        upstream.close()
        await player.wait_closed()
        await asyncio.to_thread(proxy.wait_for, r"\[initial connection\] [0-9.:]+ has disconnected", 5)

    run(body())
    address = r"\d+\.\d+\.\d+\.\d+:\d+"
    expected = [
        rf"INFO \[initial connection\] (?P<player>{address}) has connected",
        r"INFO \[server connection\] (?P=player) -> lobby has connected",
        r"INFO \[server connection\] (?P=player) -> lobby has disconnected",
        r"INFO \[initial connection\] (?P=player) has disconnected",
    ]
    events = proxy.events("INFO")
    assert re.fullmatch("\n".join(expected), "\n".join(events)), events
