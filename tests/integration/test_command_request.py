"""A player's command: a proxy command runs on the proxy, anything else reaches the backend unchanged."""

import pytest
from bedrock import Backend

NO_REASON = 40
TOO_FAST = "You are sending too many commands too quickly."


@pytest.fixture
def lobby():
    server = Backend("raknet", play=True, runtime_id=1, unique_id=-1)
    yield server
    server.close()


@pytest.fixture
def second():
    server = Backend("raknet", play=True, runtime_id=7, unique_id=-7)
    yield server
    server.close()


def proxy_for(make_proxy, lobby, second, **advanced):
    return make_proxy(
        lobby.port,
        servers={"second": f"127.0.0.1:{second.port}"},
        try_order=["lobby"],
        console=True,
        settings={"advanced": advanced} if advanced else None,
    )


def joined(player, lobby, proxy):
    assert player.wait_login(), proxy.lines[-20:]
    upstream = lobby.accept()
    assert player.peer.wait("start-game", 10) is not None, proxy.lines[-20:]
    return upstream


@pytest.mark.parametrize("line", ["/server", "server", "/SERVER"])
def test_server_lists_the_servers(make_proxy, lobby, second, connect, line):
    proxy = proxy_for(make_proxy, lobby, second)
    player = connect(proxy)
    upstream = joined(player, lobby, proxy)

    player.command(line)

    assert player.wait_text() == "§eYou are currently connected to lobby."
    assert player.wait_text() == "§eAvailable servers: §alobby§7, §7second"
    player.command("/after")
    assert upstream.wait_command() == "/after"


def test_server_moves_the_player(make_proxy, lobby, second, connect):
    proxy = proxy_for(make_proxy, lobby, second)
    player = connect(proxy)
    upstream = joined(player, lobby, proxy)

    player.command("/server second")

    downstream = second.accept()
    assert downstream.peer.wait("initialized", 10) == "7", proxy.lines[-20:]
    assert upstream.wait_closed()
    assert upstream.peer.pending("command") == []


def test_a_command_the_player_may_not_use_reaches_the_backend(make_proxy, lobby, second, connect):
    proxy = proxy_for(make_proxy, lobby, second)
    player = connect(proxy)
    upstream = joined(player, lobby, proxy)

    player.command("/glist")

    assert upstream.wait_command() == "/glist"
    assert player.texts() == []


def test_an_unknown_command_reaches_the_backend_unchanged(make_proxy, lobby, second, connect):
    proxy = proxy_for(make_proxy, lobby, second)
    player = connect(proxy)
    upstream = joined(player, lobby, proxy)

    player.command("/tp @s ~ ~ ~")
    player.command("say  Hello  World")
    player.command("/say /server")

    assert [upstream.wait_command() for _ in range(3)] == ["/tp @s ~ ~ ~", "say  Hello  World", "/say /server"]
    assert player.texts() == []


def test_a_rate_limited_command_is_forwarded(make_proxy, lobby, second, connect):
    proxy = proxy_for(make_proxy, lobby, second, **{"command-rate-limit": "600000"})
    player = connect(proxy)
    upstream = joined(player, lobby, proxy)

    player.command("/server")
    assert player.wait_text() == "§eYou are currently connected to lobby."
    player.texts()
    player.command("/server")

    assert upstream.wait_command() == "/server"
    assert player.texts() == []


def test_a_rate_limited_command_runs_on_the_proxy_when_forwarding_is_off(make_proxy, lobby, second, connect):
    proxy = proxy_for(
        make_proxy,
        lobby,
        second,
        **{"command-rate-limit": "600000", "forward-commands-if-rate-limited": "false"},
    )
    player = connect(proxy)
    upstream = joined(player, lobby, proxy)

    player.command("/server")
    player.command("/server")

    assert len(player.texts(1.0)) == 4
    assert upstream.peer.pending("command") == []


def test_a_player_is_kicked_after_too_many_rate_limited_commands(make_proxy, lobby, second, connect):
    proxy = proxy_for(
        make_proxy,
        lobby,
        second,
        **{"command-rate-limit": "600000", "kick-after-rate-limited-commands": "2"},
    )
    player = connect(proxy)
    upstream = joined(player, lobby, proxy)

    for _ in range(3):
        player.command("/tp")
    assert player.peer.wait("disconnect", 1) is None
    player.command("/tp")

    assert player.wait_disconnect() == (NO_REASON, TOO_FAST)
    assert [upstream.wait_command() for _ in range(3)] == ["/tp"] * 3


def test_log_command_executions_logs_a_players_command(make_proxy, lobby, second, connect):
    proxy = proxy_for(make_proxy, lobby, second, **{"log-command-executions": "true"})
    player = connect(proxy)
    joined(player, lobby, proxy)

    player.command("/server")

    assert proxy.wait_for(rf"INFO \[connected player\] {player.identity.name} .* -> executed command /server$", 10), (
        proxy.lines[-20:]
    )


def test_a_players_command_is_not_logged_by_default(make_proxy, lobby, second, connect):
    proxy = proxy_for(make_proxy, lobby, second)
    player = connect(proxy)
    upstream = joined(player, lobby, proxy)

    player.command("/server")
    player.command("/after")

    assert upstream.wait_command() == "/after"
    assert not proxy.events("executed command")
