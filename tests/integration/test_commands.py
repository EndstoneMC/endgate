"""Velocity's command layer driven from the console: glist, send, endgate, shutdown and what the console may not run."""

import time

import pytest
from bedrock import Backend, PlayerIdentity
from harness import Proxy

NO_REASON = 40
UNKNOWN = r"This command does not exist\."
PROXY_SHUTTING_DOWN = "Proxy shutting down."


@pytest.fixture
def lobby():
    server = Backend("raknet", play=True, runtime_id=1, unique_id=-1)
    yield server
    server.close()


@pytest.fixture
def second():
    server = Backend("raknet", play=True, runtime_id=2, unique_id=-2)
    yield server
    server.close()


def proxy_for(make_proxy, lobby, second, **options):
    return make_proxy(
        lobby.port,
        servers={"second": f"127.0.0.1:{second.port}"},
        try_order=["lobby"],
        console=True,
        **options,
    )


def joined(player, backend, proxy):
    assert player.wait_login(), proxy.lines[-20:]
    session = backend.accept()
    assert player.peer.wait("start-game", 10) is not None, proxy.lines[-20:]
    return session


def run(proxy, line: str, reply: str) -> list[str]:
    before = len(proxy.events(reply))
    proxy.command(line)
    deadline = time.monotonic() + 10
    while len(proxy.events(reply)) <= before and time.monotonic() < deadline:
        time.sleep(0.01)
    assert len(proxy.events(reply)) > before, proxy.lines[-20:]
    return proxy.events(reply)


def test_glist_counts_the_players_on_the_proxy(make_proxy, lobby, second, connect):
    proxy = proxy_for(make_proxy, lobby, second)

    run(proxy, "glist", r"0 players are currently connected to the proxy\.")
    assert proxy.events(r"To view all players on servers, use /glist all\.")

    player = connect(proxy)
    joined(player, lobby, proxy)

    run(proxy, "glist", r"1 player is currently connected to the proxy\.")


def test_glist_all_lists_each_server_that_has_players(make_proxy, lobby, second, connect):
    proxy = proxy_for(make_proxy, lobby, second)
    player = connect(proxy)
    joined(player, lobby, proxy)

    run(proxy, "glist all", r"1 player is currently connected to the proxy\.")

    assert proxy.events(rf"\[lobby\] \(1\): {player.identity.name}$")
    assert not proxy.events(r"\[second\]")


def test_glist_a_server_lists_its_players(make_proxy, lobby, second, connect):
    proxy = proxy_for(make_proxy, lobby, second)
    player = connect(proxy)
    joined(player, lobby, proxy)

    run(proxy, "glist lobby", rf"\[lobby\] \(1\): {player.identity.name}$")
    run(proxy, "glist second", r"\[second\] \(0\): $")


def test_glist_an_unknown_server_is_refused(make_proxy, lobby, second):
    proxy = proxy_for(make_proxy, lobby, second)

    run(proxy, "glist nowhere", r"The specified server nowhere does not exist\.")


def test_send_moves_a_player_to_the_other_backend(make_proxy, lobby, second, connect):
    proxy = proxy_for(make_proxy, lobby, second)
    player = connect(proxy)
    upstream = joined(player, lobby, proxy)

    proxy.command(f"send {player.identity.name} second")

    assert second.accept(10) is not None, proxy.lines[-20:]
    assert upstream.wait_closed()
    run(proxy, "glist second", rf"\[second\] \(1\): {player.identity.name}$")


def test_send_all_moves_every_player(make_proxy, lobby, second, connect):
    proxy = proxy_for(make_proxy, lobby, second)
    first = connect(proxy, identity=PlayerIdentity())
    other = connect(proxy, identity=PlayerIdentity())
    joined(first, lobby, proxy)
    joined(other, lobby, proxy)

    proxy.command("send all second")

    assert second.accept(10) is not None, proxy.lines[-20:]
    assert second.accept(10) is not None, proxy.lines[-20:]


def test_send_an_unknown_player_is_refused(make_proxy, lobby, second):
    proxy = proxy_for(make_proxy, lobby, second)

    run(proxy, "send nobody second", r"ERROR The specified player nobody does not exist\.")


def test_send_to_an_unknown_server_is_refused(make_proxy, lobby, second, connect):
    proxy = proxy_for(make_proxy, lobby, second)
    player = connect(proxy)
    joined(player, lobby, proxy)

    run(proxy, f"send {player.identity.name} nowhere", r"The specified server nowhere does not exist\.")

    assert second.sessions.empty()
    assert player.wait_disconnect(0.5) is None


def test_send_without_arguments_shows_its_usage(make_proxy, lobby, second):
    proxy = proxy_for(make_proxy, lobby, second)

    run(proxy, "send", r"/send <player> <server>")
    run(proxy, "send someone", r"/send <player> <server>")


def test_send_current_is_for_players_only(make_proxy, lobby, second, connect):
    proxy = proxy_for(make_proxy, lobby, second)
    player = connect(proxy)
    joined(player, lobby, proxy)

    run(proxy, "send current second", r"Only players can run this command\.")

    assert second.sessions.empty()


def test_endgate_info_prints_the_version(make_proxy, lobby, second):
    proxy = proxy_for(make_proxy, lobby, second)

    run(proxy, "endgate info", r"INFO Endgate \S+")
    assert proxy.events(r"licensed under the terms of the GNU Affero General Public License v3\.")
    assert proxy.events(r"https://endstone\.dev - https://github\.com/EndstoneMC/endgate")
    assert not proxy.events(chr(27))
    assert not proxy.events(chr(0xA7))


def test_endgate_alone_lists_its_subcommands(make_proxy, lobby, second):
    proxy = proxy_for(make_proxy, lobby, second)

    run(proxy, "endgate", r"/endgate <info\|reload>")


def test_endgate_reload_reads_the_configuration_again(make_proxy, lobby, second, connect):
    proxy = proxy_for(make_proxy, lobby, second)
    path = proxy.workdir / "endgate.toml"
    path.write_text(path.read_text().replace('try = ["lobby"]', 'try = ["second"]'))

    run(proxy, "endgate reload", r"Endgate configuration successfully reloaded\.")

    player = connect(proxy)
    joined(player, second, proxy)
    assert lobby.sessions.empty()


def test_endgate_reload_of_a_broken_file_is_refused(make_proxy, lobby, second):
    proxy = proxy_for(make_proxy, lobby, second)
    path = proxy.workdir / "endgate.toml"
    path.write_text(path.read_text().replace("[advanced]", "[advanced\n"))

    run(proxy, "endgate reload", r"Unable to reload your Endgate configuration\. Check the console")


def test_shutdown_tells_the_players_why(binary, tmp_path, lobby, connect):
    proxy = Proxy(binary, tmp_path / "proxy", lobby.port, console=True).start()
    try:
        player = connect(proxy)
        joined(player, lobby, proxy)

        proxy.command("shutdown")

        assert player.wait_disconnect() == (NO_REASON, PROXY_SHUTTING_DOWN)
        assert proxy.process.wait(15) == 0
    finally:
        proxy.stop()


def test_shutdown_with_a_reason_delivers_it(binary, tmp_path, lobby, connect):
    proxy = Proxy(binary, tmp_path / "proxy", lobby.port, console=True).start()
    try:
        player = connect(proxy)
        joined(player, lobby, proxy)

        proxy.command("shutdown back in five minutes")

        assert player.wait_disconnect() == (NO_REASON, "back in five minutes")
        assert proxy.process.wait(15) == 0
    finally:
        proxy.stop()


@pytest.mark.parametrize("alias", ["end", "stop"])
def test_shutdown_answers_to_its_aliases(binary, tmp_path, lobby, connect, alias):
    proxy = Proxy(binary, tmp_path / "proxy", lobby.port, console=True).start()
    try:
        player = connect(proxy)
        joined(player, lobby, proxy)

        proxy.command(alias)

        assert player.wait_disconnect() == (NO_REASON, PROXY_SHUTTING_DOWN)
        assert proxy.process.wait(15) == 0
    finally:
        proxy.stop()


def test_an_unknown_command_gets_the_unknown_command_reply(make_proxy, lobby, second):
    proxy = proxy_for(make_proxy, lobby, second)

    run(proxy, "frobnicate now", r"ERROR This command does not exist\.")


def test_commands_are_matched_without_regard_to_case(make_proxy, lobby, second):
    proxy = proxy_for(make_proxy, lobby, second)

    run(proxy, "GLIST", r"0 players are currently connected to the proxy\.")


def test_server_is_not_a_console_command(make_proxy, lobby, second):
    proxy = proxy_for(make_proxy, lobby, second)

    run(proxy, "server second", UNKNOWN)

    assert not proxy.events(r"You are currently connected")


def test_console_commands_are_not_logged_by_default(make_proxy, lobby, second):
    proxy = proxy_for(make_proxy, lobby, second)

    run(proxy, "glist", r"currently connected to the proxy")

    assert not proxy.events(r"CONSOLE -> executed command")


def test_console_commands_are_logged_when_enabled(make_proxy, lobby, second):
    proxy = proxy_for(make_proxy, lobby, second, settings={"advanced": {"log-command-executions": "true"}})

    run(proxy, "glist", r"CONSOLE -> executed command /glist$")
