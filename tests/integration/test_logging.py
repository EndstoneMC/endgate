def exchange(proxy, connect, backend) -> None:
    player = connect(proxy)
    assert player.wait_login()
    upstream = backend.accept()
    player.text("hello")
    assert upstream.wait_text() == "hello"
    player.close()


def test_player_connections_are_logged(make_proxy, backend, connect):
    proxy = make_proxy(backend.port)
    exchange(proxy, connect, backend)
    assert proxy.wait_for(r"INFO \[connected player\] .* has disconnected", 10), proxy.lines[-20:]
    assert proxy.events(r"INFO \[connected player\] .* has connected")
    assert proxy.events(r"INFO \[server connection\] .* -> lobby has connected")
    assert proxy.events(r"INFO \[server connection\] .* -> lobby has disconnected")


def test_log_player_connections_off_keeps_them_quiet(make_proxy, backend, connect):
    proxy = make_proxy(backend.port, debug=True, log_player_connections=False)
    exchange(proxy, connect, backend)
    assert proxy.wait_for(r"DEBUG \d+\.\d+\.\d+\.\d+:\d+: connection ended", 10), proxy.lines[-20:]
    assert not proxy.events("has connected|has disconnected")
