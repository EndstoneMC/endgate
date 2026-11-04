"""A player is only sent to a backend on its own protocol: at login, on failover and by the console."""

import pytest
from bedrock import Backend

OLD_PROTOCOL = 2193
NO_REASON = 40
REFUSED = "§cUnable to connect to old: it runs Minecraft 1.26.50, not 1.26.60"
NO_SERVER = "No server here runs Minecraft 1.26.60. Try again later or contact an admin."


@pytest.fixture
def lobby():
    server = Backend("raknet", play=True, runtime_id=1, unique_id=-1)
    yield server
    server.close()


@pytest.fixture
def third():
    server = Backend("raknet", play=True, runtime_id=3, unique_id=-3)
    yield server
    server.close()


@pytest.fixture
def old():
    server = Backend("raknet", play=True, protocol=OLD_PROTOCOL, runtime_id=2, unique_id=-2)
    yield server
    server.close()


def refusals(proxy):
    return proxy.events(r"disconnected while connecting to old: it runs Minecraft 1.26.50, not 1.26.60")


def test_a_player_reaches_only_the_backend_on_its_protocol(make_proxy, lobby, old, connect):
    proxy = make_proxy(lobby.port, servers={"old": f"127.0.0.1:{old.port}"}, try_order=["old", "lobby"])
    player = connect(proxy)

    assert player.wait_login(), proxy.lines[-20:]
    lobby.accept()
    assert player.peer.wait("start-game", 10) is not None, proxy.lines[-20:]
    assert old.sessions.empty()
    assert len(refusals(proxy)) == 1


def test_a_backend_known_to_be_on_another_protocol_is_not_tried_again(make_proxy, lobby, old, connect):
    proxy = make_proxy(lobby.port, servers={"old": f"127.0.0.1:{old.port}"}, try_order=["old", "lobby"])
    first = connect(proxy)
    assert first.wait_login(), proxy.lines[-20:]
    lobby.accept()
    assert first.peer.wait("start-game", 10) is not None, proxy.lines[-20:]

    second = connect(proxy)

    assert second.wait_login(), proxy.lines[-20:]
    lobby.accept()
    assert second.peer.wait("start-game", 10) is not None, proxy.lines[-20:]
    assert old.sessions.empty()
    assert len(refusals(proxy)) == 1


def test_a_pinned_backend_on_another_protocol_is_skipped(make_proxy, lobby, old, connect):
    proxy = make_proxy(lobby.port, servers={"old": f"raknet://127.0.0.1:{old.port}"}, try_order=["old", "lobby"])
    player = connect(proxy)

    assert player.wait_login(), proxy.lines[-20:]
    lobby.accept()
    assert player.peer.wait("start-game", 10) is not None, proxy.lines[-20:]
    assert old.sessions.empty()
    assert len(refusals(proxy)) == 1


def test_send_to_a_backend_on_another_protocol_is_refused_and_the_player_stays(make_proxy, lobby, old, connect):
    proxy = make_proxy(lobby.port, servers={"old": f"127.0.0.1:{old.port}"}, try_order=["lobby"], console=True)
    player = connect(proxy)
    assert player.wait_login(), proxy.lines[-20:]
    upstream = lobby.accept()
    assert player.peer.wait("start-game", 10) is not None, proxy.lines[-20:]

    proxy.command(f"send {player.identity.name} old")

    assert player.wait_text() == REFUSED, proxy.lines[-20:]
    assert old.sessions.empty()
    assert not upstream.closed.is_set()
    assert player.peer.pending("disconnect") == []


def test_failover_skips_a_backend_on_another_protocol(make_proxy, lobby, old, third, connect):
    proxy = make_proxy(
        lobby.port,
        servers={"old": f"127.0.0.1:{old.port}", "third": f"127.0.0.1:{third.port}"},
        try_order=["lobby", "old", "third"],
        console=True,
    )
    player = connect(proxy)
    assert player.wait_login(), proxy.lines[-20:]
    upstream = lobby.accept()
    assert player.peer.wait("start-game", 10) is not None, proxy.lines[-20:]
    proxy.command(f"send {player.identity.name} old")
    assert player.wait_text() == REFUSED, proxy.lines[-20:]

    upstream.close()

    third.accept()
    assert player.wait_text() == "§cYou were kicked from lobby: An internal server connection error occurred."
    assert old.sessions.empty()
    assert len(refusals(proxy)) == 1


def test_a_login_with_no_backend_on_its_protocol_is_told_so(make_proxy, old, connect):
    proxy = make_proxy(old.port)
    first = connect(proxy)

    assert first.wait_login(), proxy.lines[-20:]
    assert first.wait_disconnect() == (NO_REASON, NO_SERVER), proxy.lines[-20:]
    second = connect(proxy)

    assert second.wait_disconnect() == (NO_REASON, NO_SERVER), proxy.lines[-20:]
    assert second.peer.pending("play-status") == []
    assert old.sessions.empty()


def test_players_on_two_versions_each_reach_the_backend_on_their_own(make_proxy, old, lobby, connect):
    proxy = make_proxy(lobby.port, servers={"old": f"127.0.0.1:{old.port}"}, try_order=["old", "lobby"])
    old_player = connect(proxy, protocol=OLD_PROTOCOL)
    new_player = connect(proxy)

    assert old_player.wait_login(), proxy.lines[-20:]
    old_session = old.accept()
    assert old_player.peer.wait("start-game", 10) == "2 -2 0 0", proxy.lines[-20:]
    assert new_player.wait_login(), proxy.lines[-20:]
    new_session = lobby.accept()
    assert new_player.peer.wait("start-game", 10) == "1 -1 0 0", proxy.lines[-20:]

    old_player.say("from the old player")
    new_player.say("from the new player")
    assert old_session.wait_text() == "from the old player"
    assert new_session.wait_text() == "from the new player"
    old_session.say("to the old player")
    new_session.say("to the new player")
    assert old_player.texts() == ["to the old player"]
    assert new_player.texts() == [REFUSED, "to the new player"]
    assert old.sessions.empty()
    assert lobby.sessions.empty()
