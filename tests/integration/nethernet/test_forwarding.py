import re

from bedrock import noise
from nethernet import SendType


def join(proxy, backend, connect):
    """A NetherNet player on the proxy, and the session the proxy opened to the RakNet backend for it."""
    player = connect(proxy, "nethernet")
    assert player.wait_login()
    upstream = backend.accept(10)
    assert proxy.wait_for("-> lobby has connected", 5), proxy.lines
    return player, upstream


def test_a_batch_reaches_the_backend_with_the_raknet_prefix(proxy, backend, connect):
    player, upstream = join(proxy, backend, connect)
    player.text("hello")
    assert upstream.wait_text() == "hello"


def test_a_backend_batch_reaches_the_player_without_the_prefix(proxy, backend, connect):
    player, upstream = join(proxy, backend, connect)
    upstream.text("world")
    assert player.wait_text() == "world"


def test_a_backend_message_without_the_prefix_is_dropped(proxy, backend, connect):
    player, upstream = join(proxy, backend, connect)
    upstream.link.send(b"\x03stray")
    upstream.text("next")
    assert player.wait_text() == "next"
    assert player.texts() == []


def test_large_batches_cross_both_ways(proxy, backend, connect):
    player, upstream = join(proxy, backend, connect)
    for size in (1, 1400, 65535, 262143, 262144, 300000, 1_000_000):
        data = noise(size, size)
        player.text(data)
        assert upstream.wait_text(20) == data, size
        upstream.text(data[::-1])
        assert player.wait_text(20) == data[::-1], size


def test_a_burst_keeps_its_order_both_ways(proxy, backend, connect):
    player, upstream = join(proxy, backend, connect)
    sent = [f"{i:04d}" + noise(1024, i) for i in range(200)]
    for data in sent:
        player.text(data)
    assert [upstream.wait_text(20) for _ in sent] == sent
    for data in sent:
        upstream.text(data[::-1])
    assert [player.wait_text(20) for _ in sent] == [data[::-1] for data in sent]


def test_an_unreliable_batch_is_forwarded(proxy, backend, connect):
    player, upstream = join(proxy, backend, connect)
    player.say("movement", reliability=SendType.UNRELIABLE)
    assert upstream.wait_text() == "movement"


def test_closing_the_player_closes_the_backend(proxy, backend, connect):
    player, upstream = join(proxy, backend, connect)
    player.close()
    assert upstream.wait_closed(10)


def test_closing_the_backend_closes_the_player(proxy, backend, connect):
    player, upstream = join(proxy, backend, connect)
    upstream.close()
    assert player.wait_closed(15)


def test_the_player_is_logged_like_a_raknet_player(proxy, backend, connect):
    player, upstream = join(proxy, backend, connect)
    upstream.close()
    assert player.wait_closed(15)
    assert proxy.wait_for(r"\[connected player\] .* has disconnected", 5)
    address = r"\d+\.\d+\.\d+\.\d+:\d+"
    name = re.escape(player.identity.name)
    expected = [
        rf"INFO \[connected player\] {name} \((?P<player>{address})\) has connected",
        rf"INFO \[server connection\] {name} -> lobby has connected",
        rf"INFO \[server connection\] {name} -> lobby has disconnected",
        rf"INFO \[connected player\] {name} \((?P=player)\) has disconnected",
    ]
    events = proxy.events("INFO")
    assert re.fullmatch("\n".join(expected), "\n".join(events)), events
