"""A player whose client data names a forced host is sent to that host's servers before try."""

import pytest
from bedrock import Backend

OLD_PROTOCOL = 2193
HOST = "factions.example.com"


@pytest.fixture
def lobby():
    server = Backend("raknet", play=True, runtime_id=1, unique_id=-1)
    yield server
    server.close()


@pytest.fixture
def factions():
    server = Backend("raknet", play=True, runtime_id=2, unique_id=-2)
    yield server
    server.close()


@pytest.fixture
def old():
    server = Backend("raknet", play=True, protocol=OLD_PROTOCOL, runtime_id=3, unique_id=-3)
    yield server
    server.close()


def forced(host: str, servers: str) -> dict[str, dict[str, str]]:
    return {"forced-hosts": {f'"{host}"': servers}}


def joins(player, proxy, backend) -> bool:
    assert player.wait_login(), proxy.lines[-20:]
    backend.accept()
    return player.peer.wait("start-game", 10) is not None


@pytest.mark.parametrize("transport", ["raknet", "nethernet"])
def test_a_forced_host_sends_the_player_to_its_server(make_proxy, lobby, factions, connect, transport):
    proxy = make_proxy(
        lobby.port, servers={"factions": f"127.0.0.1:{factions.port}"}, settings=forced(HOST, '["factions"]')
    )
    player = connect(proxy, transport, server_address=f"{HOST}:19132")

    assert joins(player, proxy, factions), proxy.lines[-20:]
    assert lobby.sessions.empty()


@pytest.mark.parametrize(
    "address", ["Factions.Example.COM:19132", "factions.example.com.:19132", "factions.example.com"]
)
def test_the_host_is_lowercased_and_stripped_of_its_port_and_trailing_dot(
    make_proxy, lobby, factions, connect, address
):
    proxy = make_proxy(
        lobby.port, servers={"factions": f"127.0.0.1:{factions.port}"}, settings=forced(HOST, '"factions"')
    )
    player = connect(proxy, server_address=address)

    assert joins(player, proxy, factions), proxy.lines[-20:]
    assert lobby.sessions.empty()


@pytest.mark.parametrize("address", ["play.example.com:19132", ""])
def test_any_other_host_goes_to_try(make_proxy, lobby, factions, connect, address):
    proxy = make_proxy(
        lobby.port, servers={"factions": f"127.0.0.1:{factions.port}"}, settings=forced(HOST, '["factions"]')
    )
    player = connect(proxy, server_address=address)

    assert joins(player, proxy, lobby), proxy.lines[-20:]
    assert factions.sessions.empty()


def test_a_player_without_a_server_address_goes_to_try(make_proxy, lobby, factions, connect):
    proxy = make_proxy(
        lobby.port, servers={"factions": f"127.0.0.1:{factions.port}"}, settings=forced(HOST, '["factions"]')
    )
    player = connect(proxy)

    assert joins(player, proxy, lobby), proxy.lines[-20:]
    assert factions.sessions.empty()


def test_a_forced_host_skips_a_server_on_another_protocol(make_proxy, lobby, factions, old, connect):
    proxy = make_proxy(
        lobby.port,
        servers={"old": f"127.0.0.1:{old.port}", "factions": f"127.0.0.1:{factions.port}"},
        settings=forced(HOST, '["old", "factions"]'),
    )
    player = connect(proxy, server_address=HOST)

    assert joins(player, proxy, factions), proxy.lines[-20:]
    assert old.sessions.empty()
    assert lobby.sessions.empty()


def test_a_forced_host_whose_servers_run_another_protocol_does_not_fall_back_to_try(make_proxy, lobby, old, connect):
    proxy = make_proxy(lobby.port, servers={"old": f"127.0.0.1:{old.port}"}, settings=forced(HOST, '["old"]'))
    player = connect(proxy, server_address=HOST)

    disconnect = player.wait_disconnect(15)

    assert disconnect is not None, proxy.lines[-20:]
    assert "No server here runs Minecraft 1.26.60" in disconnect[1]
    assert lobby.sessions.empty()
    assert old.sessions.empty()


def test_a_forced_host_naming_an_unknown_server_stops_the_proxy_from_starting(make_proxy, lobby):
    with pytest.raises(RuntimeError, match="Server 'nowhere' for forced host 'factions.example.com' does not exist"):
        make_proxy(lobby.port, settings=forced(HOST, '["nowhere"]'))
