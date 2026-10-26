import hashlib
import time

import pytest
import raknet

USER = bytes([0xFE])


def digest(messages: list[bytes]) -> str:
    h = hashlib.sha256()
    for message in messages:
        h.update(len(message).to_bytes(4, "big") + message)
    return h.hexdigest()[:16]


def drain(connection, count: int, timeout: float = 10.0) -> list[bytes]:
    messages = []
    deadline = time.monotonic() + timeout
    while len(messages) < count and time.monotonic() < deadline:
        try:
            messages.append(connection.recv(timeout=max(deadline - time.monotonic(), 0.01)))
        except TimeoutError:
            break
    return messages


@pytest.fixture
def pair(proxy, backend):
    player = raknet.create_connection(proxy.address, timeout=5)
    upstream = backend.accept(timeout=5)
    time.sleep(0.05)
    yield player, upstream
    player.close()


def test_mtu_and_latency_are_negotiated(pair, backend, observe):
    player, upstream = pair
    observe("player_mtu", player._peer.raw.get_mtu_size(player._raw_address))
    observe("backend_mtu", backend.raw.get_mtu_size(upstream._raw_address))
    observe("player_sees_proxy", player.remote_address)


@pytest.mark.parametrize("size", [1, 100, 1000, 1400, 1500, 1600, 5000, 65535, 300_000])
def test_messages_of_every_size_cross_both_ways(pair, observe, size):
    player, upstream = pair
    payload = USER + bytes((i * 7) & 0xFF for i in range(size))
    player.send(payload)
    reply = USER + payload[1:][::-1]
    upstream.send(reply)
    to_backend = drain(upstream, 1)
    to_player = drain(player, 1)
    observe("to_backend", [len(m) for m in to_backend])
    observe("to_player", [len(m) for m in to_player])
    assert to_backend == [payload]
    assert to_player == [reply]


def test_burst_keeps_its_order(pair, observe):
    player, upstream = pair
    messages = [USER + i.to_bytes(4, "big") + bytes(i % 300) for i in range(2000)]
    for message in messages:
        player.send(message)
    got = drain(upstream, len(messages))
    observe("count", len(got))
    observe("in_order", got == messages)
    back = [USER + m[1:][::-1] for m in messages[:500]]
    for message in back:
        upstream.send(message)
    returned = drain(player, len(back))
    observe("returned_in_order", returned == back)


RELIABILITIES = [
    "UNRELIABLE",
    "UNRELIABLE_SEQUENCED",
    "RELIABLE",
    "RELIABLE_ORDERED",
    "RELIABLE_SEQUENCED",
    "UNRELIABLE_WITH_ACK_RECEIPT",
    "RELIABLE_WITH_ACK_RECEIPT",
    "RELIABLE_ORDERED_WITH_ACK_RECEIPT",
]


@pytest.mark.parametrize("name", RELIABILITIES)
def test_every_reliability_from_the_library(pair, observe, name):
    player, upstream = pair
    reliability = getattr(raknet.PacketReliability, name)
    messages = [USER + bytes([RELIABILITIES.index(name), i]) for i in range(50)]
    for message in messages:
        player.send(message, reliability=reliability)
    got = drain(upstream, len(messages), timeout=3)
    observe("count", len(got))
    observe("digest", digest(sorted(got)))


def test_channels_are_flattened_onto_one_order(pair, observe):
    player, upstream = pair
    messages = [USER + bytes([i % 32, i]) for i in range(256)]
    for message in messages:
        player.send(message, channel=message[1])
    got = drain(upstream, len(messages))
    observe("count", len(got))
    observe("set_equal", sorted(got) == sorted(messages))


def test_player_close_closes_backend(pair, proxy, observe):
    player, upstream = pair
    player.close()
    try:
        upstream.recv(timeout=5)
        outcome = "message"
    except raknet.ConnectionClosedOK:
        outcome = "closed_ok"
    except raknet.ConnectionClosedError:
        outcome = "closed_error"
    except TimeoutError:
        outcome = "timeout"
    observe("backend", outcome)


def test_backend_close_closes_player(pair, observe):
    player, upstream = pair
    upstream.close()
    try:
        player.recv(timeout=5)
        outcome = "message"
    except raknet.ConnectionClosedOK:
        outcome = "closed_ok"
    except raknet.ConnectionClosedError:
        outcome = "closed_error"
    except TimeoutError:
        outcome = "timeout"
    observe("player", outcome)


def test_players_come_and_go(proxy, backend, observe):
    outcomes = []
    for round_ in range(5):
        player = raknet.create_connection(proxy.address, timeout=5)
        upstream = backend.accept(timeout=5)
        time.sleep(0.05)
        player.send(USER + bytes([round_]))
        outcomes.append(drain(upstream, 1) == [USER + bytes([round_])])
        player.close()
        try:
            upstream.recv(timeout=5)
        except raknet.ConnectionClosed:
            pass
    observe("delivered", outcomes)


def test_throughput(pair, observe):
    player, upstream = pair
    chunk = USER + bytes(range(256)) * 4
    count = 4000
    started = time.monotonic()
    for _ in range(count):
        player.send(chunk)
    got = drain(upstream, count, timeout=60)
    elapsed = time.monotonic() - started
    observe("count", len(got))
    observe("metric:upstream_mib_per_s", round(len(chunk) * len(got) / elapsed / (1 << 20), 2))
