"""The proxy's commands in the backend's AvailableCommandsPacket, and their soft enums kept current."""

import commandwire
import pytest
from bedrock import Backend, PlayerIdentity
from harness import free_port


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


def rewrite(proxy, *changes: tuple[str, str]) -> None:
    path = proxy.workdir / "endgate.toml"
    text = path.read_text()
    for old, new in changes:
        assert old in text, old
        text = text.replace(old, new)
    path.write_text(text)


def test_a_player_gets_the_backends_commands_and_server(make_proxy, lobby, second, connect):
    proxy = proxy_for(make_proxy, lobby, second)
    player = connect(proxy)
    session = joined(player, lobby, proxy)
    sent = commandwire.backend()

    session.send_available_commands(sent)
    received = player.wait_available_commands()

    assert received is not None, proxy.lines[-20:]
    assert [item["name"] for item in received["commands"]] == ["help", "list", "say", "server"]
    server = commandwire.find(received, "server")
    assert server["flags"] == 128
    assert server["permission"] == "any"
    assert server["alias_enum"] == -1
    assert [(p["name"], p["optional"]) for p in server["overloads"][0]["params"]] == [("server", True)]
    symbol = server["overloads"][0]["params"][0]["parse_symbol"]
    assert symbol == commandwire.SOFT_ENUM_SYMBOL | len(sent["soft_enums"])
    assert commandwire.soft_enum(received, "ServerNames") == ["lobby", "second"]
    assert "server" in commandwire.enum_values(received, "CommandName")
    assert commandwire.find(received, "glist") is None
    assert commandwire.find(received, "send") is None
    assert commandwire.find(received, "shutdown") is None


def test_the_backends_tables_only_grow(make_proxy, lobby, second, connect):
    proxy = proxy_for(make_proxy, lobby, second)
    player = connect(proxy)
    session = joined(player, lobby, proxy)
    sent = commandwire.backend()

    session.send_available_commands(sent)
    received = player.wait_available_commands()

    assert received is not None, proxy.lines[-20:]
    assert received["enum_values"][: len(sent["enum_values"])] == sent["enum_values"]
    assert received["enums"][: len(sent["enums"]) - 1] == sent["enums"][:-1]
    assert (
        received["enums"][len(sent["enums"]) - 1]["values"][: len(sent["enums"][-1]["values"])]
        == sent["enums"][-1]["values"]
    )
    assert received["soft_enums"][: len(sent["soft_enums"])] == sent["soft_enums"]
    for item in sent["commands"]:
        assert commandwire.find(received, item["name"]) == item


def test_a_backend_command_of_the_same_name_is_replaced(make_proxy, lobby, second, connect):
    proxy = proxy_for(make_proxy, lobby, second)
    player = connect(proxy)
    session = joined(player, lobby, proxy)
    sent = commandwire.backend(("help", "server"))

    session.send_available_commands(sent)
    received = player.wait_available_commands()

    assert received is not None, proxy.lines[-20:]
    names = [item["name"] for item in received["commands"]]
    assert names == ["help", "server"]
    assert commandwire.find(received, "server") != commandwire.find(sent, "server")
    assert commandwire.find(received, "server")["flags"] == 128


def test_announce_proxy_commands_off_leaves_the_packet_alone(make_proxy, lobby, second, connect):
    proxy = proxy_for(make_proxy, lobby, second, settings={"advanced": {"announce-proxy-commands": "false"}})
    player = connect(proxy)
    session = joined(player, lobby, proxy)
    sent = commandwire.backend()

    session.send_available_commands(sent)
    received = player.wait_available_commands()

    assert received == sent
    assert player.wait_soft_enum(0.5) is None


def test_a_reload_that_adds_a_server_updates_the_soft_enum(make_proxy, lobby, second, connect):
    proxy = proxy_for(make_proxy, lobby, second)
    player = connect(proxy)
    session = joined(player, lobby, proxy)
    session.send_available_commands(commandwire.backend())
    assert player.wait_available_commands() is not None, proxy.lines[-20:]

    rewrite(proxy, ("[advanced]", f'third = "127.0.0.1:{free_port()}"\n[advanced]'))
    proxy.command("endgate reload")

    assert player.wait_soft_enum() == ("ServerNames", 0, ["third"]), proxy.lines[-20:]
    assert player.wait_soft_enum(0.5) is None


def test_a_player_joining_changes_no_soft_enum_a_player_holds(make_proxy, lobby, second, connect):
    proxy = proxy_for(make_proxy, lobby, second)
    first = connect(proxy)
    session = joined(first, lobby, proxy)
    session.send_available_commands(commandwire.backend())
    assert first.wait_available_commands() is not None, proxy.lines[-20:]

    other = connect(proxy, identity=PlayerIdentity())
    joined(other, lobby, proxy)

    assert first.wait_soft_enum(1.0) is None
    assert commandwire.soft_enum(commandwire.backend(), "PlayerNames") is None


def test_a_switch_injects_the_next_backends_packet(make_proxy, lobby, second, connect):
    proxy = proxy_for(make_proxy, lobby, second)
    player = connect(proxy)
    upstream = joined(player, lobby, proxy)
    upstream.send_available_commands(commandwire.backend())
    assert player.wait_available_commands() is not None, proxy.lines[-20:]

    proxy.command(f"send {player.identity.name} second")
    downstream = second.accept(10)
    assert downstream is not None, proxy.lines[-20:]
    assert downstream.peer.wait("initialized", 10) == "2", proxy.lines[-20:]
    downstream.send_available_commands(commandwire.backend(("list", "ban")))
    received = player.wait_available_commands()

    assert received is not None, proxy.lines[-20:]
    assert [item["name"] for item in received["commands"]] == ["list", "ban", "server"]
    assert commandwire.soft_enum(received, "ServerNames") == ["lobby", "second"]
