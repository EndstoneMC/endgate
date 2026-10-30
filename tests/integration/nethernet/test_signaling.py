import json
import re
import socket
import ssl
import subprocess
import time
import urllib.request

import pytest
from harness import REPOSITORY

SUPPORTED = [
    (int(protocol), name)
    for protocol, name in re.findall(
        r'ProtocolVersion\{(\d+), "([^"]+)"\}', (REPOSITORY / "src/endgate/network/version.h").read_text()
    )
]


def ping(url: str, query: str = "", context: ssl.SSLContext | None = None) -> tuple[str, dict]:
    with urllib.request.urlopen(f"{url}/v1/join{query}", timeout=5, context=context) as response:
        assert response.status == 200
        return response.headers["Content-Type"], json.loads(response.read())


def test_the_ping_shows_a_supported_client_its_own_protocol(proxy):
    for protocol, name in SUPPORTED:
        content_type, body = ping(proxy.url, f"?version={name}&protocol={protocol}&platform=8&id=1")
        assert content_type == "application/json"
        assert (body["protocol"], body["version"]) == (protocol, name)


def test_the_ping_shows_any_other_client_the_newest_protocol(proxy):
    for query in ("", "?version=1.26.70.21&protocol=2238&platform=8&id=1", "?protocol=oops"):
        _, body = ping(proxy.url, query)
        assert (body["protocol"], body["version"]) == SUPPORTED[-1], query


def test_the_ping_announces_the_motd_and_the_player_cap(proxy):
    _, body = ping(proxy.url)
    assert (body["name"], body["players"], body["maxPlayers"]) == ("Endgate Test", 0, proxy.max_players)


def test_tls_is_refused_at_once_without_a_certificate(proxy):
    context = ssl.create_default_context()
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE
    started = time.monotonic()
    with socket.create_connection(proxy.address, timeout=5) as raw:
        with pytest.raises(ssl.SSLError):
            context.wrap_socket(raw, server_hostname="localhost")
    assert time.monotonic() - started < 1


def test_tls_is_served_with_the_configured_certificate(make_proxy, backend, tmp_path):
    certificate, key = tmp_path / "fullchain.pem", tmp_path / "privkey.pem"
    subprocess.run(
        ["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256", "-nodes"]
        + ["-subj", "/CN=localhost", "-days", "1", "-keyout", str(key), "-out", str(certificate)],
        check=True,
        capture_output=True,
    )
    proxy = make_proxy(backend.port, tls=(certificate, key))
    context = ssl.create_default_context(cafile=certificate)
    context.check_hostname = False
    _, secure = ping(f"https://127.0.0.1:{proxy.port}", context=context)
    _, plain = ping(proxy.url)
    assert secure == plain
