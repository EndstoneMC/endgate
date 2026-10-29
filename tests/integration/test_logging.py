import raknet

BATCH = bytes([0xFE])


def exchange(proxy) -> None:
    player = raknet.create_connection(proxy.address, timeout=5)
    player.send(BATCH + b"hello")
    assert player.recv(timeout=10) == BATCH + b"hello"
    player.close()


def test_player_connections_are_logged(make_proxy, echo_backend):
    proxy = make_proxy(echo_backend.port)
    exchange(proxy)
    assert proxy.wait_for(r"INFO \[initial connection\] .* has disconnected", 10), proxy.lines[-20:]
    assert proxy.events(r"INFO \[initial connection\] .* has connected")
    assert proxy.events(r"INFO \[server connection\] .* -> lobby has connected")
    assert proxy.events(r"INFO \[server connection\] .* -> lobby has disconnected")


def test_log_player_connections_off_keeps_them_quiet(make_proxy, echo_backend):
    proxy = make_proxy(echo_backend.port, debug=True, log_player_connections=False)
    exchange(proxy)
    assert proxy.wait_for(r"DEBUG \d+\.\d+\.\d+\.\d+:\d+: connection ended", 10), proxy.lines[-20:]
    assert not proxy.events("has connected|has disconnected")
