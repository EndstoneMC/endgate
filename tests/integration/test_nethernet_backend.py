import pytest
from bedrock import noise

SIZES = (1, 1400, 65535, 300000, 1_000_000)


@pytest.mark.parametrize("transport", ["raknet", "nethernet"])
def test_a_player_reaches_a_nethernet_backend(make_proxy, nethernet_backend, connect, transport):
    proxy = make_proxy(nethernet_backend.port, debug=True)
    player = connect(proxy, transport)
    assert player.wait_login()
    upstream = nethernet_backend.accept(20)
    for size in SIZES:
        message = noise(size, size)
        player.text(message)
        assert upstream.wait_text(30) == message, size
        upstream.text(message[::-1])
        assert player.wait_text(30) == message[::-1], size
    assert proxy.events("DEBUG Server lobby uses NetherNet")
    assert proxy.wait_for(r"INFO \[server connection\] .* -> lobby has connected")


def test_a_burst_to_a_nethernet_backend_keeps_its_order(make_proxy, nethernet_backend, connect):
    proxy = make_proxy(nethernet_backend.port)
    player = connect(proxy)
    assert player.wait_login()
    upstream = nethernet_backend.accept(20)
    assert proxy.wait_for(r"INFO \[server connection\] .* -> lobby has connected", 20), proxy.lines[-20:]
    sent = [f"{i:04d}" + noise(1024, i) for i in range(500)]
    for message in sent:
        player.text(message)
    assert [upstream.wait_text(20) for _ in sent] == sent


def test_closing_the_player_closes_the_nethernet_backend_leg(make_proxy, nethernet_backend, connect):
    proxy = make_proxy(nethernet_backend.port)
    player = connect(proxy)
    assert player.wait_login()
    upstream = nethernet_backend.accept(20)
    player.text("hello")
    assert upstream.wait_text(20) == "hello"
    player.close()
    assert proxy.wait_for(r"INFO \[server connection\] .* -> lobby has disconnected", 10), proxy.lines[-20:]
    assert upstream.wait_closed(10)
