"""Forwarding a player's identity to a backend, and the key file that lets the backend trust it."""

import base64
import json
import re

import pytest
from bedrock import PlayerIdentity
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import encode_dss_signature
from harness import Proxy

AUDIENCE = "api://auth-minecraft-services/multiplayer"
MODERN = {"": {"player-info-forwarding-mode": '"modern"'}}


def unbase64url(text: str) -> bytes:
    return base64.urlsafe_b64decode(text + "=" * (-len(text) % 4))


def identity_key(proxy):
    pem = (proxy.workdir / "keys" / "server_identity_key.pem").read_bytes()
    return serialization.load_pem_private_key(pem, None)


def spki(key) -> str:
    der = key.public_key().public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)
    return base64.b64encode(der).decode()


def logged_in(player, backend, proxy):
    assert player.wait_login(), proxy.lines[-20:]
    session = backend.accept()
    assert proxy.wait_for(r"INFO \[server connection\] .* has connected", 10), proxy.lines[-20:]
    return session


def split(jwt: str):
    header, claims, signature = jwt.split(".")
    return json.loads(unbase64url(header)), json.loads(unbase64url(claims)), unbase64url(signature), header, claims


def test_modern_forwarding_signs_the_players_own_token_with_endgates_key(make_proxy, backend, services, connect):
    proxy = make_proxy(backend.port, services=services, settings=MODERN)
    identity = PlayerIdentity(name="Alex", xuid="2535400000000042")
    player = connect(proxy, services=services, identity=identity)
    upstream = logged_in(player, backend, proxy)

    assert upstream.peer.wait("login-type") == "0"
    header, claims, signature, header_part, claims_part = split(upstream.peer.wait("login-jwt"))
    key = identity_key(proxy)

    assert header == {"alg": "ES384", "kid": spki(key)}
    assert claims["aud"] == AUDIENCE
    assert claims["iss"] == services.ISSUER
    assert claims["xid"] == "2535400000000042"
    assert claims["xname"] == "Alex"
    assert claims["cpk"] == spki(key)
    assert claims["cpk"] != identity.cpk
    assert claims["exp"] > claims["iat"]
    assert claims["endgate:player_info"] == {"version": 1, "address": "127.0.0.1"}

    r, s = int.from_bytes(signature[:48], "big"), int.from_bytes(signature[48:], "big")
    key.public_key().verify(
        encode_dss_signature(r, s), f"{header_part}.{claims_part}".encode(), ec.ECDSA(hashes.SHA384())
    )


def test_the_forwarded_login_is_a_token_and_not_a_self_signed_one_only_in_modern_mode(
    make_proxy, backend, services, connect
):
    proxy = make_proxy(backend.port, services=services)
    player = connect(proxy, services=services, identity=PlayerIdentity())
    upstream = logged_in(player, backend, proxy)
    header, claims, *_ = split(upstream.peer.wait("login-jwt"))
    assert upstream.peer.wait("login-type") == "2"
    assert "kid" not in header
    assert "aud" not in claims
    assert "endgate:player_info" not in claims


def test_only_the_forwarding_token_carries_aud_and_kid(make_proxy, backend, services, connect):
    proxy = make_proxy(backend.port, services=services, settings=MODERN)
    player = connect(proxy, services=services, identity=PlayerIdentity())
    logged_in(player, backend, proxy)
    header, claims = player.peer.wait("handshake-token").split(" ", 1)
    assert set(json.loads(header)) == {"alg", "x5u"}
    assert set(json.loads(claims)) == {"salt"}


def test_modern_forwarding_in_offline_mode_refuses_to_start(binary, tmp_path, backend):
    proxy = Proxy(binary, tmp_path / "proxy", backend.port, settings=MODERN)
    assert proxy._launch() is False
    assert proxy.process.wait(10) != 0
    assert proxy.events(r"ERROR Modern player info forwarding needs online-mode")
    assert proxy.events(r"ERROR Your configuration is invalid")


def test_the_public_half_of_the_identity_key_is_written_once(make_proxy, backend):
    proxy = make_proxy(backend.port)
    path = proxy.workdir / "keys" / "forwarding_key.pem"
    text = path.read_text()
    assert text.startswith("-----BEGIN PUBLIC KEY-----")
    der = serialization.load_pem_public_key(text.encode()).public_bytes(
        serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo
    )
    assert base64.b64encode(der).decode() == spki(identity_key(proxy))
    assert any(
        re.search(r"INFO The forwarding-key-file does not exist\. A new file has been created at ", line)
        for line in proxy.lines
    )


def test_a_forwarding_key_file_holding_another_key_is_kept_and_warned_about(binary, tmp_path, backend):
    workdir = tmp_path / "proxy"
    (workdir / "keys").mkdir(parents=True)
    other = (
        ec.generate_private_key(ec.SECP384R1())
        .public_key()
        .public_bytes(serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo)
    )
    (workdir / "keys" / "forwarding_key.pem").write_bytes(other)
    proxy = Proxy(binary, workdir, backend.port).start()
    try:
        assert (workdir / "keys" / "forwarding_key.pem").read_bytes() == other
        assert proxy.events(r"WARN The forwarding-key-file at .* holds a different key")
        assert not any("A new file has been created" in line for line in proxy.lines)
    finally:
        proxy.stop()


@pytest.mark.parametrize("name", ["custom.pem"])
def test_forwarding_key_file_names_where_the_key_goes(make_proxy, backend, name):
    proxy = make_proxy(backend.port, settings={"": {"forwarding-key-file": f'"{name}"'}})
    assert (proxy.workdir / name).read_text().startswith("-----BEGIN PUBLIC KEY-----")
