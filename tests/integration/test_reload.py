"""The console's reload reads endgate.toml again: new servers, try order, forced hosts, and what happens to players."""

import pytest
from bedrock import Backend

HOST = "factions.example.com"
RELOADED = r"successfully reloaded"
FAILED = r"Unable to reload your Endgate configuration"


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


def rewrite(proxy, *changes: tuple[str, str]) -> None:
    path = proxy.workdir / "endgate.toml"
    text = path.read_text()
    for old, new in changes:
        assert old in text, old
        text = text.replace(old, new)
    path.write_text(text)


def reload(proxy, outcome: str = RELOADED) -> None:
    before = len(proxy.events(outcome))
    proxy.command("endgate reload")
    assert proxy.wait_for(outcome, 10) and len(proxy.events(outcome)) > before, proxy.lines[-20:]


def joins(player, proxy, backend) -> bool:
    assert player.wait_login(), proxy.lines[-20:]
    backend.accept()
    return player.peer.wait("start-game", 10) is not None


def test_a_new_server_becomes_reachable(make_proxy, lobby, second, connect):
    proxy = make_proxy(lobby.port, console=True)
    player = connect(proxy)
    assert joins(player, proxy, lobby)

    rewrite(proxy, ("[advanced]", f'second = "127.0.0.1:{second.port}"\n[advanced]'))
    reload(proxy)
    proxy.command(f"send {player.identity.name} second")

    assert second.accept(10) is not None, proxy.lines[-20:]


def test_a_changed_try_order_applies_to_the_next_player(make_proxy, lobby, second, connect):
    proxy = make_proxy(lobby.port, servers={"second": f"127.0.0.1:{second.port}"}, console=True)
    rewrite(proxy, ('try = ["lobby"]', 'try = ["second"]'))
    reload(proxy)

    player = connect(proxy)
    assert joins(player, proxy, second), proxy.lines[-20:]
    assert lobby.sessions.empty()


def test_a_new_forced_host_applies_to_the_next_player(make_proxy, lobby, second, connect):
    proxy = make_proxy(lobby.port, servers={"second": f"127.0.0.1:{second.port}"}, console=True)
    rewrite(proxy, ("[advanced]", f'[forced-hosts]\n"{HOST}" = ["second"]\n[advanced]'))
    reload(proxy)

    player = connect(proxy, server_address=f"{HOST}:19132")
    assert joins(player, proxy, second), proxy.lines[-20:]
    assert lobby.sessions.empty()


def test_an_invalid_configuration_is_refused_and_the_old_one_stays(make_proxy, lobby, second, connect):
    proxy = make_proxy(lobby.port, servers={"second": f"127.0.0.1:{second.port}"}, console=True)
    rewrite(proxy, ('try = ["lobby"]', 'try = ["second"]'), ("compression-level = -1", "compression-level = 42"))
    reload(proxy, FAILED)

    assert proxy.events(r"ERROR Invalid compression level 42")
    player = connect(proxy)
    assert joins(player, proxy, lobby), proxy.lines[-20:]
    assert second.sessions.empty()


def test_an_unreadable_configuration_is_refused(make_proxy, lobby):
    proxy = make_proxy(lobby.port, console=True)
    rewrite(proxy, ("[advanced]", "[advanced\n"))
    reload(proxy, FAILED)

    assert proxy.events(r"ERROR Unable to reload configuration")


def test_a_connected_player_stays_connected(make_proxy, lobby, connect):
    proxy = make_proxy(lobby.port, console=True)
    player = connect(proxy)
    assert joins(player, proxy, lobby)

    rewrite(proxy, ('motd = "Endgate Test"', 'motd = "Reloaded"'))
    reload(proxy)

    assert player.wait_disconnect(1.0) is None
    assert proxy.alive()


def test_a_server_whose_address_changed_moves_its_players(make_proxy, lobby, second, connect):
    proxy = make_proxy(lobby.port, console=True)
    player = connect(proxy)
    assert joins(player, proxy, lobby)

    rewrite(proxy, (f'lobby = "127.0.0.1:{lobby.port}"', f'lobby = "127.0.0.1:{second.port}"'))
    reload(proxy)

    assert second.accept(10) is not None, proxy.lines[-20:]
    assert player.wait_disconnect(1.0) is None


def test_a_player_who_cannot_be_moved_is_disconnected(make_proxy, lobby, connect):
    proxy = make_proxy(lobby.port, console=True)
    player = connect(proxy)
    assert joins(player, proxy, lobby)

    rewrite(proxy, (f'lobby = "127.0.0.1:{lobby.port}"', 'lobby = "127.0.0.1:1"'))
    reload(proxy)

    assert player.wait_disconnect(30) is not None, proxy.lines[-20:]
