"""What the proxy logs about a player's address, and how it shuts down with players connected."""

import re

import pytest
from bedrock import PlayerIdentity
from harness import Proxy

NO_REASON = 40
ADDRESS = r"\d+\.\d+\.\d+\.\d+:\d+"
WITHHELD = "<ip address withheld>"


def logged_in(player, backend, proxy):
    assert player.wait_login(), proxy.lines[-20:]
    session = backend.accept()
    assert proxy.wait_for(r"INFO \[server connection\] .* has connected", 10), proxy.lines[-20:]
    return session


def exchange(proxy, backend, connect):
    player = connect(proxy)
    upstream = logged_in(player, backend, proxy)
    player.text("hello")
    assert upstream.wait_text() == "hello"
    player.close()
    assert proxy.wait_for(r"DEBUG .*: connection ended", 10), proxy.lines[-20:]
    assert proxy.wait_for(r"INFO \[connected player\] .* has disconnected", 10), proxy.lines[-20:]


def test_player_addresses_are_logged_by_default(make_proxy, backend, connect):
    proxy = make_proxy(backend.port, debug=True)
    exchange(proxy, backend, connect)
    assert proxy.events(rf"INFO \[connected player\] .* \({ADDRESS}\) has connected")
    assert proxy.events(rf"INFO \[connected player\] .* \({ADDRESS}\) has disconnected")
    assert proxy.events(rf"DEBUG {ADDRESS}: connection ended")
    assert not proxy.events(WITHHELD)


def test_player_addresses_are_logged_when_enabled(make_proxy, backend, connect):
    proxy = make_proxy(backend.port, debug=True, settings={"": {"enable-player-address-logging": "true"}})
    exchange(proxy, backend, connect)
    assert proxy.events(rf"INFO \[connected player\] .* \({ADDRESS}\) has connected")
    assert not proxy.events(WITHHELD)


def test_player_addresses_are_withheld_when_disabled(make_proxy, backend, connect):
    proxy = make_proxy(backend.port, debug=True, settings={"": {"enable-player-address-logging": "false"}})
    exchange(proxy, backend, connect)
    assert proxy.events(rf"INFO \[connected player\] .* \({re.escape(WITHHELD)}\) has connected")
    assert proxy.events(rf"INFO \[connected player\] .* \({re.escape(WITHHELD)}\) has disconnected")
    assert proxy.events(rf"DEBUG {re.escape(WITHHELD)}: connection ended")
    assert not proxy.events(ADDRESS), proxy.events(ADDRESS)


def test_player_addresses_are_withheld_for_a_nethernet_player(make_proxy, backend, connect):
    proxy = make_proxy(backend.port, debug=True, settings={"": {"enable-player-address-logging": "false"}})
    player = connect(proxy, "nethernet")
    logged_in(player, backend, proxy)
    player.close()
    assert proxy.wait_for(r"INFO \[connected player\] .* has disconnected", 10), proxy.lines[-20:]
    assert proxy.events(rf"INFO \[connected player\] .* \({re.escape(WITHHELD)}\) has connected")
    assert not proxy.events(ADDRESS), proxy.events(ADDRESS)


def test_shutdown_kicks_every_player_before_closing(binary, tmp_path, backend, connect):
    proxy = Proxy(binary, tmp_path / "proxy", backend.port, console=True).start()
    try:
        raknet = connect(proxy, identity=PlayerIdentity())
        nethernet = connect(proxy, "nethernet", identity=PlayerIdentity())
        logged_in(raknet, backend, proxy)
        logged_in(nethernet, backend, proxy)

        proxy.command("shutdown")

        assert raknet.wait_disconnect() == (NO_REASON, "Proxy shutting down.")
        assert nethernet.wait_disconnect() == (NO_REASON, "Proxy shutting down.")
        assert raknet.wait_closed()
        assert nethernet.wait_closed()
        assert proxy.process.wait(15) == 0
        assert proxy.events(r"INFO Shutting down the proxy")
        assert not proxy.events(r"ERROR")
    finally:
        proxy.stop()


@pytest.mark.parametrize("command", ["end", "stop"])
def test_the_console_aliases_shut_down_too(binary, tmp_path, backend, connect, command):
    proxy = Proxy(binary, tmp_path / "proxy", backend.port, console=True).start()
    try:
        player = connect(proxy)
        logged_in(player, backend, proxy)
        proxy.command(command)
        assert player.wait_disconnect() == (NO_REASON, "Proxy shutting down.")
        assert proxy.process.wait(15) == 0
    finally:
        proxy.stop()
