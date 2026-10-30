import concurrent.futures


def test_raknet_and_nethernet_players_share_the_port(make_proxy, backend, connect):
    proxy = make_proxy(backend.port)
    transports = ["nethernet", "nethernet", "raknet", "raknet"]
    with concurrent.futures.ThreadPoolExecutor(len(transports)) as pool:
        players = list(pool.map(lambda transport: connect(proxy, transport), transports))
    for index, player in enumerate(players):
        assert player.wait_login()
        player.text(f"{transports[index]} {index}")
    heard = sorted(backend.accept(20).wait_text(10) for _ in players)
    assert heard == ["nethernet 0", "nethernet 1", "raknet 2", "raknet 3"]
    assert len(proxy.events(r"INFO \[connected player\] .* has connected")) == 4
    assert len(proxy.events(r"INFO \[server connection\] .* -> lobby has connected")) == 4
