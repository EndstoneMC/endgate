from __future__ import annotations

import concurrent.futures
import socket
import struct

import pytest
from harness import Proxy, free_port

HANDSHAKE = 0x09
STAT = 0x00
FULL_PADDING = b"splitnum\x00\x80\x00"
PLAYER_PADDING = b"\x01player_\x00\x00"


class Query:
    """A GameSpy 4 client."""

    def __init__(self, address: tuple[str, int], timeout: float = 5.0) -> None:
        self.address = address
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.settimeout(timeout)

    def close(self) -> None:
        self.socket.close()

    def request(self, kind: int, session: int, payload: bytes = b"") -> bytes | None:
        self.socket.sendto(b"\xfe\xfd" + bytes([kind]) + struct.pack(">i", session) + payload, self.address)
        try:
            return self.socket.recvfrom(4096)[0]
        except TimeoutError:
            return None

    def handshake(self, session: int = 1) -> int:
        reply = self.request(HANDSHAKE, session)
        assert reply is not None
        assert reply[0] == HANDSHAKE
        assert struct.unpack(">i", reply[1:5])[0] == session
        assert reply.endswith(b"\x00")
        return int(reply[5:-1])

    def basic(self, session: int = 2, token: int | None = None) -> bytes | None:
        token = self.handshake(session) if token is None else token
        return self.request(STAT, session, struct.pack(">i", token))

    def full(self, session: int = 3, token: int | None = None) -> bytes | None:
        token = self.handshake(session) if token is None else token
        return self.request(STAT, session, struct.pack(">i", token) + b"\x00\x00\x00\x00")


def parse_basic(reply: bytes, session: int) -> dict[str, object]:
    assert reply[0] == STAT
    assert struct.unpack(">i", reply[1:5])[0] == session
    fields = reply[5:].split(b"\x00", 5)
    port = struct.unpack("<H", fields[5][:2])[0]
    return {
        "hostname": fields[0].decode(),
        "gametype": fields[1].decode(),
        "map": fields[2].decode(),
        "numplayers": int(fields[3]),
        "maxplayers": int(fields[4]),
        "hostport": port,
        "hostip": fields[5][2:-1].decode(),
    }


def parse_full(reply: bytes, session: int) -> tuple[dict[str, str], list[str]]:
    assert reply[0] == STAT
    assert struct.unpack(">i", reply[1:5])[0] == session
    body = reply[5:]
    assert body.startswith(FULL_PADDING)
    body = body[len(FULL_PADDING) :]
    head, _, tail = body.partition(b"\x00\x01player_\x00\x00")
    parts = head.split(b"\x00")
    properties = {parts[i].decode(): parts[i + 1].decode() for i in range(0, len(parts) - 1, 2)}
    players = [name.decode() for name in tail.split(b"\x00") if name]
    return properties, players


class SharedPort:
    """The proxy's bind port, known only once it has picked one."""

    proxy: Proxy | None = None

    def __str__(self) -> str:
        assert self.proxy is not None
        return str(self.proxy.port)


@pytest.fixture
def started(binary, backend, tmp_path):
    proxies: list[Proxy] = []

    def start(query: dict[str, str] | None, **options) -> Proxy:
        settings = {"query": query} if query is not None else None
        proxy = Proxy(binary, tmp_path / f"proxy{len(proxies)}", backend.port, settings=settings, **options)
        for value in (query or {}).values():
            if isinstance(value, SharedPort):
                value.proxy = proxy
        proxies.append(proxy.start())
        return proxy

    yield start
    for proxy in proxies:
        assert proxy.alive(), "endgate exited during the test:\n" + "\n".join(proxy.lines[-40:])
        assert proxy.stop() == 0


@pytest.fixture
def client():
    clients: list[Query] = []

    def make(address: tuple[str, int], timeout: float = 5.0) -> Query:
        clients.append(Query(address, timeout))
        return clients[-1]

    yield make
    for query in clients:
        query.close()


def shared_settings() -> dict[str, str]:
    return {"enabled": "true", "port": SharedPort(), "map": '"Lobby"'}


def test_handshake_gets_a_token(started, client):
    proxy = started(shared_settings())
    query = client(proxy.address)
    first = query.handshake(7)
    second = query.handshake(8)
    assert -(2**31) <= first < 2**31
    assert first != second


def test_basic_stat(started, client):
    proxy = started(shared_settings())
    reply = client(proxy.address).basic(0x01020304)
    assert reply is not None
    assert parse_basic(reply, 0x01020304) == {
        "hostname": "Endgate Test",
        "gametype": "SMP",
        "map": "Lobby",
        "numplayers": 0,
        "maxplayers": 10,
        "hostport": proxy.port,
        "hostip": "127.0.0.1",
    }


def test_full_stat(started, client):
    proxy = started(shared_settings())
    reply = client(proxy.address).full(0x0A0B0C0D)
    assert reply is not None
    properties, players = parse_full(reply, 0x0A0B0C0D)
    assert properties == {
        "hostname": "Endgate Test",
        "gametype": "SMP",
        "game_id": "MINECRAFTPE",
        "version": properties["version"],
        "plugins": "Endgate: ",
        "map": "Lobby",
        "numplayers": "0",
        "maxplayers": "10",
        "hostport": str(proxy.port),
        "hostip": "127.0.0.1",
    }
    assert properties["version"].count(".") == 2
    assert players == []


def test_wrong_token_is_ignored(started, client):
    proxy = started(shared_settings())
    query = client(proxy.address, timeout=0.5)
    token = query.handshake()
    assert query.basic(token=token + 1) is None
    assert query.full(token=token + 1) is None
    assert query.basic(token=token) is not None


def test_stat_without_handshake_is_ignored(started, client):
    proxy = started(shared_settings())
    assert client(proxy.address, timeout=0.5).request(STAT, 1, struct.pack(">i", 0)) is None


def test_truncated_and_unknown_queries_are_ignored(started, client):
    proxy = started(shared_settings())
    query = client(proxy.address, timeout=0.5)
    query.socket.sendto(b"\xfe\xfd\x09", proxy.address)
    assert query.request(0x42, 1) is None
    assert query.request(STAT, 1, b"\x00\x00") is None
    assert query.handshake() is not None


def test_query_does_not_answer_by_default(started, client):
    proxy = started(None)
    assert client(proxy.address, timeout=0.5).request(HANDSHAKE, 1) is None


def test_query_does_not_answer_when_disabled(started, client):
    proxy = started({"enabled": "false", "port": SharedPort()})
    assert client(proxy.address, timeout=0.5).request(HANDSHAKE, 1) is None


def test_query_on_its_own_port(started, client):
    port = free_port()
    proxy = started({"enabled": "true", "port": str(port)})
    reply = client(("127.0.0.1", port)).basic()
    assert reply is not None
    assert parse_basic(reply, 2)["hostport"] == proxy.port
    assert client(proxy.address, timeout=0.5).request(HANDSHAKE, 1) is None


def test_players_join_while_the_port_is_queried(started, client, backend, connect):
    proxy = started(shared_settings())
    query = client(proxy.address)
    transports = ["nethernet", "raknet", "nethernet", "raknet"]

    def join(transport: str):
        return connect(proxy, transport)

    with concurrent.futures.ThreadPoolExecutor(len(transports)) as pool:
        joining = [pool.submit(join, transport) for transport in transports]
        for session in range(20):
            assert query.basic(session) is not None
        players = [future.result() for future in joining]
    for player in players:
        assert player.wait_login()
        player.text("hello")
    heard = [backend.accept(20).wait_text(10) for _ in players]
    assert heard == ["hello"] * len(players)
    reply = query.full()
    assert reply is not None
    properties, names = parse_full(reply, 3)
    assert properties["numplayers"] == str(len(players))
    assert len(names) == len(players)
