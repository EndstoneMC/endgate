"""A player logging in through the proxy, over each transport and each way the proxy can authenticate it."""

import hashlib
import json
import uuid

import pytest
from bedrock import PlayerIdentity, sign_token

LOGIN_FAILED_CLIENT_OLD = 1
NO_REASON = 40
DISCONNECT_NOT_AUTHENTICATED = 46
DISCONNECT_EXPIRED_TOKEN = 144


def logged_in(player, backend, proxy):
    """The backend's session for the player, once both legs are up."""
    assert player.wait_login(), proxy.lines[-20:]
    session = backend.accept()
    assert proxy.wait_for(r"INFO \[server connection\] .* has connected", 10), proxy.lines[-20:]
    return session


def test_a_raknet_player_logs_in_offline_and_chats_with_the_backend(proxy, backend, connect):
    player = connect(proxy)
    upstream = logged_in(player, backend, proxy)
    player.text("hello backend")
    assert upstream.wait_text() == "hello backend"
    upstream.text("hello player")
    assert player.wait_text() == "hello player"
    assert player.peer.pending("handshake") == []


def test_a_nethernet_player_logs_in_offline_and_chats_with_the_backend(proxy, backend, connect):
    player = connect(proxy, "nethernet")
    upstream = logged_in(player, backend, proxy)
    player.text("hello backend")
    assert upstream.wait_text() == "hello backend"
    upstream.text("hello player")
    assert player.wait_text() == "hello player"
    assert player.peer.pending("handshake") == []


def test_a_raknet_player_is_encrypted_in_online_mode(make_proxy, backend, services, connect):
    proxy = make_proxy(backend.port, services=services)
    player = connect(proxy, services=services)
    upstream = logged_in(player, backend, proxy)
    assert player.peer.wait("handshake", 0) is not None, "the client never finished the encryption handshake"
    player.text("sealed")
    assert upstream.wait_text() == "sealed"
    upstream.text("sealed back")
    assert player.wait_text() == "sealed back"


def test_a_raknet_player_and_a_raknet_backend_are_both_encrypted(make_proxy, encrypted_backend, services, connect):
    proxy = make_proxy(encrypted_backend.port, services=services)
    player = connect(proxy, services=services)
    upstream = logged_in(player, encrypted_backend, proxy)
    assert upstream.peer.wait("handshake", 5) is not None
    for size in (1, 1000, 100_000, 400_000):
        message = chr(ord("a") + size % 26) * size
        player.text(message)
        assert upstream.wait_text(20) == message
        upstream.text(message[::-1] + "!")
        assert player.wait_text(20) == message[::-1] + "!"


@pytest.mark.parametrize("transport", ["raknet", "nethernet"])
def test_the_backend_sees_the_player_s_name_in_a_login_signed_by_the_proxy(proxy, backend, connect, transport):
    player = connect(proxy, transport)
    upstream = logged_in(player, backend, proxy)
    token = json.loads(upstream.peer.wait("login-token"))
    client_data = json.loads(upstream.peer.wait("client-data"))
    assert token["xname"] == player.identity.name
    assert client_data["ThirdPartyName"] == player.identity.name
    assert client_data["DeviceModel"] == "EndgateTest"
    assert token["cpk"] != player.identity.cpk


def test_the_backend_sees_the_authenticated_name_in_online_mode(make_proxy, backend, services, connect):
    proxy = make_proxy(backend.port, services=services)
    identity = PlayerIdentity(name="Alex", xuid="2535400000000042")
    player = connect(proxy, services=services, identity=identity)
    upstream = logged_in(player, backend, proxy)
    assert json.loads(upstream.peer.wait("login-token"))["xname"] == "Alex"
    assert json.loads(upstream.peer.wait("client-data"))["ThirdPartyName"] == "Alex"


def test_the_backend_finds_the_player_under_the_id_an_online_mode_join_uses(make_proxy, backend, services, connect):
    proxy = make_proxy(backend.port, services=services)
    identity = PlayerIdentity(name="Alex", xuid="2535400000000042")
    player = connect(proxy, services=services, identity=identity)
    upstream = logged_in(player, backend, proxy)
    digest = bytearray(hashlib.md5(b"pocket-auth-1-xuid:2535400000000042").digest())
    digest[6] = digest[6] & 0x0F | 0x30
    digest[8] = digest[8] & 0x3F | 0x80
    assert json.loads(upstream.peer.wait("client-data"))["SelfSignedId"] == str(uuid.UUID(bytes=bytes(digest)))


@pytest.mark.parametrize("transport", ["raknet", "nethernet"])
def test_a_second_login_under_the_same_name_is_told_it_is_already_connected(proxy, backend, connect, transport):
    identity = PlayerIdentity()
    logged_in(connect(proxy, transport, identity=identity), backend, proxy)
    duplicate = connect(proxy, transport, identity=identity)
    assert duplicate.wait_disconnect() == (NO_REASON, "You are already connected to this proxy!")


def test_a_login_from_an_older_protocol_is_refused(proxy, connect):
    player = connect(proxy, protocol=2000)
    assert player.peer.wait("play-status", 10) == str(LOGIN_FAILED_CLIENT_OLD)
    assert player.wait_closed()


def test_a_backend_disconnect_before_the_join_reaches_the_player(proxy, backend, connect):
    player = connect(proxy)
    upstream = logged_in(player, backend, proxy)
    upstream.peer.command("disconnect " + b"go away".hex())
    assert player.wait_disconnect() == (NO_REASON, "Unable to connect to lobby: go away")


def test_closing_the_player_closes_the_backend(proxy, backend, connect):
    player = connect(proxy)
    upstream = logged_in(player, backend, proxy)
    player.close()
    assert upstream.wait_closed()
    assert proxy.wait_for(r"INFO \[server connection\] .* -> lobby has disconnected", 10), proxy.lines[-20:]


def test_closing_the_backend_closes_the_player(proxy, backend, connect):
    player = connect(proxy)
    upstream = logged_in(player, backend, proxy)
    upstream.close()
    assert player.wait_closed(15)


class TestOnlineLoginIsRefused:
    @pytest.fixture
    def online(self, make_proxy, backend, services):
        return make_proxy(backend.port, services=services)

    def refusal(self, player) -> tuple[int, str] | None:
        return player.wait_disconnect()

    def test_with_a_token_that_has_expired(self, online, services, connect):
        identity = PlayerIdentity()
        player = connect(
            online, identity=identity, request=identity.connection_request(token=services.token(identity, lifetime=-60))
        )
        assert self.refusal(player)[0] == DISCONNECT_EXPIRED_TOKEN

    def test_with_a_token_for_another_audience(self, online, services, connect):
        identity = PlayerIdentity()
        token = services.token(identity, audience="api://somewhere-else")
        player = connect(online, identity=identity, request=identity.connection_request(token=token))
        assert self.refusal(player)[0] == DISCONNECT_NOT_AUTHENTICATED

    def test_with_a_token_signed_by_another_key(self, online, services, connect):
        identity = PlayerIdentity()
        forged = PlayerIdentity()
        token = sign_token(forged.key, {"kid": services.KEY_ID}, {"aud": "api://auth-minecraft-services/multiplayer"})
        player = connect(online, identity=identity, request=identity.connection_request(token=token))
        assert self.refusal(player)[0] == DISCONNECT_NOT_AUTHENTICATED

    def test_with_a_self_signed_login(self, online, connect):
        player = connect(online)
        assert self.refusal(player)[0] == DISCONNECT_NOT_AUTHENTICATED
