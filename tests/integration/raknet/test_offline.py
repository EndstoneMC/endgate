import time

import raknet
import wire
from wire import ID, Socket


def mask_pong(described: dict, port: int) -> dict:
    payload = described.pop("payload", b"")
    if payload:
        length = int.from_bytes(payload[:2], "big")
        fields = payload[2:].decode().split(";")
        fields[6] = "<guid>"
        fields = [("<port>" if f == str(port) else f) for f in fields]
        described["motd"] = {"declared_length": length, "actual_length": len(payload) - 2, "fields": fields}
    return described


def offline(proxy, data: bytes, sock: Socket | None = None) -> list[dict]:
    own = sock is None
    sock = sock or Socket()
    try:
        sock.send(data, proxy.address)
        return [wire.describe_offline(d) for d, _ in sock.drain()]
    finally:
        if own:
            sock.close()


def test_unconnected_ping_is_answered_with_the_announcement(proxy, observe):
    for kind in (ID.UNCONNECTED_PING, ID.UNCONNECTED_PING_OPEN_CONNECTIONS):
        replies = offline(proxy, wire.unconnected_ping(0x0102030405060708, 42, kind))
        observe(kind.name, [mask_pong(r, proxy.port) for r in replies])
        assert len(replies) == 1 and replies[0]["time"] == 0x0102030405060708


def test_unconnected_ping_through_the_raknet_library(proxy, observe):
    pong = raknet.ping(proxy.address, timeout=2)
    fields = pong.data[2:].decode().split(";")
    observe("fields", fields[:6] + fields[7:10])
    assert fields[0] == "MCPE"


def test_malformed_unconnected_pings_are_ignored(proxy, observe):
    good = wire.unconnected_ping(1, 2)
    cases = {
        "bad_magic": good[:9] + bytes(16) + good[25:],
        "no_guid": good[:25],
        "truncated_magic": good[:20],
        "only_id": good[:1],
        "empty_two": good[:2],
    }
    observe("replies", {name: offline(proxy, data) for name, data in cases.items()})
    observe("logs", proxy.events())


def test_open_connection_request_1_negotiates_the_mtu(proxy, observe):
    results = {}
    for mtu in (46, 100, 576, 1200, 1492, 1500, 1600, 1601, 2028):
        results[mtu] = offline(proxy, wire.open_connection_request_1(mtu))
    observe("replies", results)
    observe("too_short", offline(proxy, wire.open_connection_request_1(0)[:17]))
    assert results[1200][0]["mtu"] == 1200


def test_open_connection_request_1_rejects_another_protocol(proxy, observe):
    for protocol in (6, 10, 12, 255):
        observe(str(protocol), offline(proxy, wire.open_connection_request_1(1200, protocol)))


def test_open_connection_request_2_outcomes(proxy, observe):
    first = Socket()
    second = Socket()
    try:
        guid = 0x1234567890
        request = wire.open_connection_request_2(proxy.address, 1200, guid)

        def masked(replies, sock):
            for reply in replies:
                if reply.get("address") == sock.address:
                    reply["address"] = "self"
            return replies

        observe("first", masked(offline(proxy, request, first), first))
        observe("duplicate", masked(offline(proxy, request, first), first))
        observe(
            "same_address_new_guid",
            offline(proxy, wire.open_connection_request_2(proxy.address, 1200, guid + 1), first),
        )
        observe("same_guid_new_address", offline(proxy, request, second))
    finally:
        first.close()
        second.close()


def test_open_connection_request_2_echoes_the_requested_mtu(proxy, observe):
    for mtu in (400, 576, 1200, 1600, 1700):
        sock = Socket()
        try:
            replies = offline(proxy, wire.open_connection_request_2(proxy.address, mtu, 1000 + mtu), sock)
            observe(str(mtu), [{k: v for k, v in r.items() if k != "address"} for r in replies])
        finally:
            sock.close()


def test_open_connection_request_2_without_a_guid(proxy, observe):
    request = wire.open_connection_request_2(proxy.address, 1200, 7)[:-8]
    sock = Socket()
    try:
        observe("replies", [{k: v for k, v in r.items() if k != "address"} for r in offline(proxy, request, sock)])
    finally:
        sock.close()


def test_half_open_connection_is_reaped(proxy, observe):
    sock = Socket()
    try:
        offline(proxy, wire.open_connection_request_2(proxy.address, 1200, 99), sock)
        observe("while_half_open", offline(proxy, wire.open_connection_request_2(proxy.address, 1200, 100), sock))
        time.sleep(10.5)
        replies = offline(proxy, wire.open_connection_request_2(proxy.address, 1200, 100), sock)
        observe("after_reap", [{k: v for k, v in r.items() if k != "address"} for r in replies])
    finally:
        sock.close()
    observe("logs", proxy.events())


def test_show_max_players_is_not_a_cap(make_proxy, backend, join_raw, observe):
    proxy = make_proxy(backend.port, max_players=1)
    first = join_raw(proxy)
    second = join_raw(proxy)
    observe("first", first.transcript)
    observe("second", second.transcript)
    assert first.last_accepted is not None
    assert second.last_accepted is not None
