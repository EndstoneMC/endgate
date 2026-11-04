import json
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import pytest
import raknet
from harness import free_port

BACKEND_MOTD = "Backend Motd"
BACKEND_LEVEL = "Backend World"
PROXY_MOTD = "Endgate Test"


def pong(protocol: int = 2225, motd: str = BACKEND_MOTD) -> bytes:
    fields = f"MCPE;{motd};{protocol};1.26.60;7;40;1;{BACKEND_LEVEL};Creative;1;19132;19133;0;"
    return len(fields).to_bytes(2, "big") + fields.encode()


class RakNetBackend:
    """A RakNet server that only announces, as BDS does with enable-lan-visibility on."""

    def __init__(self, protocol: int = 2225, motd: str = BACKEND_MOTD) -> None:
        self.server = raknet.create_server(
            ("127.0.0.1", free_port()), max_connections=1, offline_ping_response=pong(protocol, motd)
        )
        self.port = self.server.local_address[1]
        threading.Thread(target=self.server.serve_forever, args=(lambda connection: None,), daemon=True).start()

    def close(self) -> None:
        self.server.close(0)


class NetherNetBackend:
    """An HTTP server that answers GET /v1/join with a BDS ping."""

    def __init__(self, protocol: int = 2225, motd: str = BACKEND_MOTD) -> None:
        body = json.dumps(
            {
                "name": motd,
                "protocol": protocol,
                "version": "1.26.60",
                "level": BACKEND_LEVEL,
                "players": 7,
                "maxPlayers": 40,
                "gameType": 1,
            }
        ).encode()

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self) -> None:
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *args) -> None:
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.port = self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def close(self) -> None:
        self.server.shutdown()
        self.server.server_close()


@pytest.fixture
def raknet_backend():
    server = RakNetBackend()
    yield server
    server.close()


@pytest.fixture
def nethernet_backend():
    server = NetherNetBackend()
    yield server
    server.close()


def mode(value: str) -> dict[str, dict[str, str]]:
    return {"": {"ping-passthrough": f'"{value}"'}}


def raknet_ping(proxy) -> dict[str, str]:
    data = raknet.ping(proxy.address, timeout=10).data
    fields = data.decode().split(";")
    return {
        "motd": fields[1],
        "protocol": fields[2],
        "version": fields[3],
        "players": fields[4],
        "max": fields[5],
        "level": fields[7],
        "game": fields[8],
    }


def nethernet_ping(proxy, protocol: int | None = None, host: str | None = None) -> dict:
    query = f"?protocol={protocol}" if protocol else ""
    request = urllib.request.Request(f"{proxy.url}/v1/join{query}", headers={"Host": host} if host else {})
    with urllib.request.urlopen(request, timeout=10) as response:
        return json.loads(response.read())


def test_a_disabled_passthrough_answers_with_the_proxys_own_ping(make_proxy, raknet_backend):
    proxy = make_proxy(raknet_backend.port)
    ping = raknet_ping(proxy)
    assert ping["motd"] == PROXY_MOTD
    assert ping["level"] != BACKEND_LEVEL
    assert ping["max"] == "10"
    assert ping["game"] == "Survival"


def test_description_takes_the_motd_and_level_name_from_the_backend_over_raknet(make_proxy, raknet_backend):
    proxy = make_proxy(raknet_backend.port, settings=mode("DESCRIPTION"))
    ping = raknet_ping(proxy)
    assert ping["motd"] == BACKEND_MOTD
    assert ping["level"] == BACKEND_LEVEL
    assert ping["players"] == "0"
    assert ping["max"] == "10"
    assert ping["game"] == "Survival"


def test_all_takes_the_whole_announcement_from_the_backend_over_raknet(make_proxy, raknet_backend):
    proxy = make_proxy(raknet_backend.port, settings=mode("ALL"))
    ping = raknet_ping(proxy)
    assert ping["motd"] == BACKEND_MOTD
    assert ping["level"] == BACKEND_LEVEL
    assert ping["players"] == "7"
    assert ping["max"] == "40"
    assert ping["game"] == "Creative"


def test_the_mode_is_read_without_regard_to_case(make_proxy, raknet_backend):
    proxy = make_proxy(raknet_backend.port, settings=mode("all"))
    assert raknet_ping(proxy)["motd"] == BACKEND_MOTD


def test_description_takes_the_motd_and_level_name_from_the_backend_over_nethernet(make_proxy, raknet_backend):
    proxy = make_proxy(raknet_backend.port, settings=mode("DESCRIPTION"))
    ping = nethernet_ping(proxy)
    assert ping["name"] == BACKEND_MOTD
    assert ping["level"] == BACKEND_LEVEL
    assert ping["maxPlayers"] == 10
    assert ping["gameType"] == 0


def test_all_takes_the_whole_announcement_from_the_backend_over_nethernet(make_proxy, raknet_backend):
    proxy = make_proxy(raknet_backend.port, settings=mode("ALL"))
    ping = nethernet_ping(proxy)
    assert ping["name"] == BACKEND_MOTD
    assert ping["players"] == 7
    assert ping["maxPlayers"] == 40
    assert ping["gameType"] == 1


def test_a_nethernet_backend_is_passed_through_to_a_raknet_client(make_proxy, nethernet_backend):
    proxy = make_proxy(nethernet_backend.port, settings=mode("ALL"))
    ping = raknet_ping(proxy)
    assert ping["motd"] == BACKEND_MOTD
    assert ping["max"] == "40"


def test_a_nethernet_backend_is_passed_through_to_a_nethernet_client(make_proxy, nethernet_backend):
    proxy = make_proxy(nethernet_backend.port, settings=mode("DESCRIPTION"))
    ping = nethernet_ping(proxy)
    assert ping["name"] == BACKEND_MOTD
    assert ping["level"] == BACKEND_LEVEL


@pytest.mark.parametrize("value", ["DESCRIPTION", "ALL"])
def test_a_backend_that_does_not_answer_leaves_the_proxys_own_ping(make_proxy, value):
    proxy = make_proxy(free_port(), settings=mode(value))
    started = time.monotonic()
    ping = raknet_ping(proxy)
    assert time.monotonic() - started < 8
    assert ping["motd"] == PROXY_MOTD
    assert ping["max"] == "10"
    assert nethernet_ping(proxy)["name"] == PROXY_MOTD


def test_the_first_backend_that_answers_is_used(make_proxy, raknet_backend):
    proxy = make_proxy(
        raknet_backend.port,
        servers={"down": f"127.0.0.1:{free_port()}"},
        try_order=["down", "lobby"],
        settings=mode("ALL"),
    )
    assert raknet_ping(proxy)["motd"] == BACKEND_MOTD


def test_backends_are_pinged_at_once(make_proxy):
    second = RakNetBackend(2225, "Second")
    third = RakNetBackend(2225, "Third")
    try:
        proxy = make_proxy(
            free_port(),
            servers={
                "gone": f"127.0.0.1:{free_port()}",
                "second": f"127.0.0.1:{second.port}",
                "third": f"127.0.0.1:{third.port}",
            },
            try_order=["lobby", "gone", "second", "third"],
            settings=mode("ALL"),
        )
        started = time.monotonic()
        assert raknet_ping(proxy)["motd"] == "Second"
        assert time.monotonic() - started < 6
    finally:
        second.close()
        third.close()


def test_a_backend_on_another_protocol_than_the_client_is_skipped(make_proxy):
    newer = RakNetBackend(2225, "Newer")
    older = RakNetBackend(2193, "Older")
    try:
        proxy = make_proxy(
            newer.port,
            servers={"other": f"127.0.0.1:{older.port}"},
            try_order=["lobby", "other"],
            settings=mode("ALL"),
        )
        assert nethernet_ping(proxy, protocol=2193)["name"] == "Older"
        assert nethernet_ping(proxy, protocol=2225)["name"] == "Newer"
    finally:
        newer.close()
        older.close()


def test_no_backend_on_the_clients_protocol_leaves_the_proxys_own_ping(make_proxy, raknet_backend):
    proxy = make_proxy(raknet_backend.port, settings=mode("ALL"))
    ping = nethernet_ping(proxy, protocol=2193)
    assert ping["name"] == PROXY_MOTD
    assert ping["protocol"] == 2193


def test_ping_requests_are_not_logged_by_default(make_proxy, raknet_backend):
    proxy = make_proxy(raknet_backend.port)
    raknet_ping(proxy)
    nethernet_ping(proxy, protocol=2225)
    assert not proxy.events("is pinging the server")


def test_show_ping_requests_logs_each_ping(make_proxy, raknet_backend):
    proxy = make_proxy(raknet_backend.port, settings={"advanced": {"show-ping-requests": "true"}})
    raknet_ping(proxy)
    nethernet_ping(proxy, protocol=2225)
    assert proxy.wait_for(r"INFO \[initial connection\] 127\.0\.0\.1:\d+ is pinging the server with version 1\.26\.60")
    assert proxy.events(r"INFO \[initial connection\] 127\.0\.0\.1:\d+ is pinging the server with version unknown")


def test_show_ping_requests_logs_a_passed_through_ping(make_proxy, raknet_backend):
    proxy = make_proxy(
        raknet_backend.port, settings={"": {"ping-passthrough": '"ALL"'}, "advanced": {"show-ping-requests": "true"}}
    )
    assert raknet_ping(proxy)["motd"] == BACKEND_MOTD
    assert proxy.wait_for(r"INFO \[initial connection\] .* is pinging the server with version unknown")


@pytest.mark.parametrize("value", ["EVERYTHING", "MODS"])
def test_an_unknown_passthrough_mode_stops_the_proxy_from_starting(make_proxy, value):
    with pytest.raises(RuntimeError):
        make_proxy(free_port(), settings=mode(value))


def test_a_forced_host_chooses_the_backend_a_nethernet_ping_asks(make_proxy):
    lobby = RakNetBackend(2225, "Lobby")
    hub = RakNetBackend(2225, "Hub")
    try:
        proxy = make_proxy(
            lobby.port,
            servers={"hub": f"127.0.0.1:{hub.port}"},
            settings={"": {"ping-passthrough": '"ALL"'}, "forced-hosts": {'"play.example.com"': '["hub"]'}},
        )
        assert nethernet_ping(proxy, host="Play.Example.com.")["name"] == "Hub"
        assert nethernet_ping(proxy, host="other.example.com")["name"] == "Lobby"
        assert raknet_ping(proxy)["motd"] == "Lobby"
    finally:
        lobby.close()
        hub.close()
