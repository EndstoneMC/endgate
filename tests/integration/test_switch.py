"""A player moved between two backends: by the console, by failover, by a transfer, and refused by the guard."""

import pytest
from bedrock import Backend

LOBBY_RUNTIME_ID = 1
SECOND_RUNTIME_ID = 7
START_LOADING_SCREEN = 1
END_LOADING_SCREEN = 2
NETHER = 1


@pytest.fixture
def lobby():
    server = Backend("raknet", play=True, runtime_id=LOBBY_RUNTIME_ID, unique_id=-1)
    yield server
    server.close()


@pytest.fixture
def second():
    server = Backend("raknet", play=True, runtime_id=SECOND_RUNTIME_ID, unique_id=-7)
    yield server
    server.close()


@pytest.fixture
def nether():
    server = Backend("raknet", play=True, runtime_id=SECOND_RUNTIME_ID, unique_id=-7, dimension=NETHER)
    yield server
    server.close()


@pytest.fixture
def different_blocks():
    server = Backend("raknet", play=True, runtime_id=SECOND_RUNTIME_ID, unique_id=-7, checksum=99)
    yield server
    server.close()


def proxy_for(make_proxy, lobby, second):
    return make_proxy(
        lobby.port, servers={"second": f"127.0.0.1:{second.port}"}, try_order=["lobby", "second"], console=True
    )


def joined(player, lobby, proxy):
    """The player's session on lobby, once the player has spawned there."""
    assert player.wait_login(), proxy.lines[-20:]
    upstream = lobby.accept()
    assert player.peer.wait("start-game", 10) is not None, proxy.lines[-20:]
    assert upstream.peer.wait("initialized", 10) == str(LOBBY_RUNTIME_ID), proxy.lines[-20:]
    return upstream


def spawned(downstream, proxy):
    """Whether the player finished spawning on the backend the switch went to."""
    assert downstream.peer.wait("loading-screen", 10) == str(START_LOADING_SCREEN), proxy.lines[-20:]
    assert downstream.peer.wait("loading-screen", 10) == str(END_LOADING_SCREEN), proxy.lines[-20:]
    return downstream.peer.wait("initialized", 10) == str(SECOND_RUNTIME_ID)


def test_send_moves_the_player_under_a_fade(make_proxy, lobby, second, connect):
    proxy = proxy_for(make_proxy, lobby, second)
    player = connect(proxy)
    upstream = joined(player, lobby, proxy)

    proxy.command(f"send {player.identity.name} second")

    assert spawned(second.accept(), proxy)
    assert upstream.wait_closed()
    assert [player.peer.wait("camera", 10) for _ in range(3)] == ["fade 0.5 5 0.5", "clear", "fade 0 0 0.5"]
    assert player.peer.pending("start-game") == []


def test_a_switch_into_another_dimension_leaves_the_fade_to_the_camera_clear(make_proxy, lobby, nether, connect):
    proxy = proxy_for(make_proxy, lobby, nether)
    player = connect(proxy)
    upstream = joined(player, lobby, proxy)

    proxy.command(f"send {player.identity.name} second")

    assert spawned(nether.accept(), proxy)
    assert upstream.wait_closed()
    assert [player.peer.wait("camera", 10) for _ in range(2)] == ["fade 0.5 5 0.5", "clear"]
    assert player.peer.wait("change-dimension", 10) == str(NETHER)
    assert player.peer.wait("camera", 2) is None


def test_the_first_start_game_reaches_the_player_without_its_generator(make_proxy, lobby, second, connect):
    proxy = proxy_for(make_proxy, lobby, second)
    player = connect(proxy)
    assert player.wait_login(), proxy.lines[-20:]
    lobby.accept()

    assert player.peer.wait("start-game", 10) == f"{LOBBY_RUNTIME_ID} -1 0 0"


def test_a_kick_moves_the_player_to_the_next_server(make_proxy, lobby, second, connect):
    proxy = proxy_for(make_proxy, lobby, second)
    player = connect(proxy)
    upstream = joined(player, lobby, proxy)

    upstream.peer.command("kick 0 " + b"go away".hex())

    assert spawned(second.accept(), proxy)
    assert player.wait_text() == "You were kicked from lobby: go away"
    assert player.peer.pending("disconnect") == []


def test_a_server_that_drops_the_player_hands_them_on(make_proxy, lobby, second, connect):
    proxy = proxy_for(make_proxy, lobby, second)
    player = connect(proxy)
    upstream = joined(player, lobby, proxy)

    upstream.close()

    assert spawned(second.accept(), proxy)
    assert player.wait_text() == "You were kicked from lobby: An internal server connection error occurred."


def test_a_server_with_other_blocks_is_refused(make_proxy, lobby, different_blocks, connect):
    proxy = proxy_for(make_proxy, lobby, different_blocks)
    player = connect(proxy)
    upstream = joined(player, lobby, proxy)

    proxy.command(f"send {player.identity.name} second")

    refused = different_blocks.accept()
    assert player.wait_text() == "Unable to connect to second: it has other blocks than the server you joined", (
        proxy.lines[-20:]
    )
    assert refused.wait_closed(3)
    assert player.peer.wait("camera", 10) == "fade 0.5 5 0.5"
    assert player.peer.wait("camera", 10) == "fade 0 0 0.5"
    assert not upstream.closed.is_set()


def test_a_transfer_to_a_known_server_switches(make_proxy, lobby, second, connect):
    proxy = proxy_for(make_proxy, lobby, second)
    player = connect(proxy)
    upstream = joined(player, lobby, proxy)

    upstream.peer.command(f"transfer 127.0.0.1 {second.port}")

    assert spawned(second.accept(), proxy)
    assert player.peer.pending("transfer") == []


def test_a_transfer_elsewhere_reaches_the_player(make_proxy, lobby, second, connect):
    proxy = proxy_for(make_proxy, lobby, second)
    player = connect(proxy)
    upstream = joined(player, lobby, proxy)

    upstream.peer.command("transfer play.example.com 19132")

    assert player.peer.wait("transfer", 10) == "play.example.com 19132"
