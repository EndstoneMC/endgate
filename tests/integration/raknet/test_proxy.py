import hashlib
import time

import pytest
import raknet
from bedrock import noise


def digest(messages: list[str]) -> str:
    h = hashlib.sha256()
    for message in map(str.encode, messages):
        h.update(len(message).to_bytes(4, "big") + message)
    return h.hexdigest()[:16]


def drain(session, count: int, timeout: float = 10.0) -> list[str]:
    """The chat lines a session receives, until `count` have come or the time is up."""
    lines = []
    deadline = time.monotonic() + timeout
    while len(lines) < count and time.monotonic() < deadline:
        line = session.wait_text(max(deadline - time.monotonic(), 0.01))
        if line is None:
            break
        lines.append(line)
    return lines


@pytest.fixture
def pair(proxy, backend, connect):
    player = connect(proxy)
    assert player.wait_login()
    upstream = backend.accept(timeout=10)
    time.sleep(0.05)
    return player, upstream


def test_mtu_and_latency_are_negotiated(pair, backend, observe):
    player, upstream = pair
    connection = player.link.connection
    observe("player_mtu", connection._peer.raw.get_mtu_size(connection._raw_address))
    observe("backend_mtu", backend.server.raw.get_mtu_size(upstream.link.connection._raw_address))
    observe("player_sees_proxy", connection.remote_address)


@pytest.mark.parametrize("size", [1, 100, 1000, 1400, 1500, 1600, 5000, 65535, 300_000])
def test_messages_of_every_size_cross_both_ways(pair, observe, size):
    player, upstream = pair
    message = noise(size, size)
    player.text(message)
    reply = message[::-1]
    upstream.text(reply)
    to_backend = drain(upstream, 1)
    to_player = drain(player, 1)
    observe("to_backend", [len(m) for m in to_backend])
    observe("to_player", [len(m) for m in to_player])
    assert to_backend == [message]
    assert to_player == [reply]


def test_burst_keeps_its_order(pair, observe):
    player, upstream = pair
    messages = [f"{i:04d}" + "x" * (i % 300) for i in range(2000)]
    for message in messages:
        player.text(message)
    got = drain(upstream, len(messages))
    observe("count", len(got))
    observe("in_order", got == messages)
    back = [m[::-1] for m in messages[:500]]
    for message in back:
        upstream.text(message)
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
    messages = [f"{RELIABILITIES.index(name)}:{i}" for i in range(50)]
    for message in messages:
        player.say(message, reliability=reliability)
    got = drain(upstream, len(messages), timeout=3)
    observe("count", len(got))
    observe("digest", digest(sorted(got)))


def test_channels_are_flattened_onto_one_order(pair, observe):
    player, upstream = pair
    messages = [f"{i % 32}:{i}" for i in range(256)]
    for i, message in enumerate(messages):
        player.say(message, channel=i % 32)
    got = drain(upstream, len(messages))
    observe("count", len(got))
    observe("set_equal", sorted(got) == sorted(messages))


def test_player_close_closes_backend(pair, proxy, observe):
    player, upstream = pair
    player.close()
    upstream.wait_closed(5)
    observe("backend", upstream.outcome if upstream.closed.is_set() else "timeout")


def test_backend_close_closes_player(pair, observe):
    player, upstream = pair
    upstream.close()
    player.wait_closed(5)
    observe("player", player.outcome if player.closed.is_set() else "timeout")


def test_players_come_and_go(proxy, backend, connect, observe):
    outcomes = []
    for round_ in range(5):
        player = connect(proxy)
        assert player.wait_login()
        upstream = backend.accept(timeout=10)
        player.text(f"round {round_}")
        outcomes.append(drain(upstream, 1) == [f"round {round_}"])
        player.close()
        upstream.wait_closed(5)
    observe("delivered", outcomes)


def test_throughput(pair, observe):
    player, upstream = pair
    chunk = noise(1024)
    count = 4000
    started = time.monotonic()
    for _ in range(count):
        player.text(chunk)
    got = drain(upstream, count, timeout=60)
    elapsed = time.monotonic() - started
    observe("count", len(got))
    observe("metric:upstream_mib_per_s", round(len(chunk) * len(got) / elapsed / (1 << 20), 2))
