import asyncio
import time

import nethernet
from nethernet import SendType


def run(coroutine):
    return asyncio.run(asyncio.wait_for(coroutine, 60))


async def connect(echo) -> nethernet.Connection:
    return await nethernet.connect_http(echo.url, timeout=20)


def payload(size: int, seed: int) -> bytes:
    return bytes((i * 31 + seed) & 0xFF for i in range(size))


def test_a_reliable_packet_comes_back(echo):
    async def body():
        connection = await connect(echo)
        await connection.send(b"\x01hello")
        assert await asyncio.wait_for(connection.recv(), 10) == b"\x01hello"
        await connection.close()

    run(body())
    assert any(line.startswith("open ") for line in echo.lines)


def test_an_unreliable_packet_comes_back(echo):
    async def body():
        connection = await connect(echo)
        await connection.send(b"\x02movement", SendType.UNRELIABLE)
        assert await asyncio.wait_for(connection.recv(), 10) == b"\x02movement"
        await connection.close()

    run(body())


def test_packets_larger_than_one_fragment_cross_both_ways(echo):
    async def body():
        connection = await connect(echo)
        for size in (262143, 262144, 300000, 1_000_000):
            data = payload(size, size & 0xFF)
            await connection.send(data)
            assert await asyncio.wait_for(connection.recv(), 20) == data, size
        await connection.close()

    run(body())


def test_a_burst_keeps_its_order(echo):
    async def body():
        connection = await connect(echo)
        sent = [payload(1024, i) + i.to_bytes(4, "big") for i in range(200)]
        for data in sent:
            await connection.send(data)
        received = [await asyncio.wait_for(connection.recv(), 20) for _ in sent]
        assert received == sent
        await connection.close()

    run(body())


def test_two_clients_share_the_port(echo):
    async def body():
        first, second = await asyncio.gather(connect(echo), connect(echo))
        await first.send(b"\x01first")
        await second.send(b"\x01second")
        assert await asyncio.wait_for(first.recv(), 10) == b"\x01first"
        assert await asyncio.wait_for(second.recv(), 10) == b"\x01second"
        await asyncio.gather(first.close(), second.close())

    run(body())
    assert sum(line.startswith("open ") for line in echo.lines) == 2


def test_a_closed_client_is_reaped(echo):
    async def body():
        connection = await connect(echo)
        await connection.send(b"\x01bye")
        assert await asyncio.wait_for(connection.recv(), 10) == b"\x01bye"
        await connection.close()

    run(body())
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline and not any(line.startswith("closed ") for line in echo.lines):
        time.sleep(0.1)
    assert any(line.startswith("closed ") for line in echo.lines), echo.lines
