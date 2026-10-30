import time

import pytest
import wire
from bedrock import noise
from raw import RawClient
from wire import ID, Frame
from wire import Reliability as R

USER = bytes([0xFE])


def outcome(session, timeout: float) -> str:
    return session.outcome if session.wait_closed(timeout) else "timeout"


def test_handshake(player, proxy, observe):
    observe("transcript", player.transcript)
    observe("logs", [line.replace(str(player.address[1]), "<player>") for line in proxy.events()])


def test_message_round_trip(player, observe):
    observe("to_backend", player.say("hello").summary())
    observe("backend_received", player.backend.texts())
    player.backend.text("world")
    observe("to_player", player.collect().summary())
    observe("player_received", player.heard())


@pytest.mark.parametrize("reliability", list(R))
def test_each_reliability_is_forwarded(player, observe, reliability):
    observe("step", player.say(f"reliability {int(reliability)}", reliability).summary())
    observe("backend_received", player.backend.texts())


def test_ordered_messages_wait_for_the_gap(player, observe):
    session = player.session
    first = session.frame(player.encode("first"), R.RELIABLE_ORDERED, 3)
    second = session.frame(player.encode("second"), R.RELIABLE_ORDERED, 3)
    observe("second_first", player.exchange(wire.data_datagram(session.datagram_sequence + 1, [second])).summary())
    observe("held_back", player.backend.texts(0.3))
    observe("gap_filled", player.exchange(wire.data_datagram(session.datagram_sequence, [first])).summary())
    session.datagram_sequence += 2
    observe("released", player.backend.texts())


def test_sequenced_messages_drop_older(player, observe):
    session = player.session
    frames = [session.frame(player.encode(f"sequenced {i}"), R.UNRELIABLE_SEQUENCED, 5) for i in range(3)]
    for frame in (frames[2], frames[0], frames[1]):
        step = player.exchange(wire.data_datagram(session.datagram_sequence, [frame]))
        observe(f"sent_{frame.sequence_index}", step.summary())
        session.datagram_sequence += 1
    observe("backend_received", player.backend.texts())


def test_skipped_datagrams_are_naked(player, observe):
    session = player.session
    session.datagram_sequence += 3
    observe("jump", player.say("jump").summary())
    observe("backend_received", player.backend.texts())


def test_duplicate_datagram_is_acked_and_dropped(player, observe):
    session = player.session
    datagram = wire.data_datagram(
        session.datagram_sequence, [session.frame(player.encode("duplicate"), R.RELIABLE_ORDERED)]
    )
    session.datagram_sequence += 1
    observe("first", player.exchange(datagram).summary())
    observe("again", player.exchange(datagram).summary())
    observe("backend_received", player.backend.texts())


def test_split_message_is_reassembled_out_of_order(player, observe):
    session = player.session
    text = noise(4000)
    parts = session.split(player.encode(text), 500)
    assert len(parts) > 3
    order = list(range(len(parts)))
    steps = []
    for part in (parts[i] for i in order[::2][::-1] + order[1::2]):
        steps.append(player.exchange(wire.data_datagram(session.datagram_sequence, [part])).summary())
        session.datagram_sequence += 1
    observe("steps", steps)
    got = player.backend.texts()
    observe("backend_received", [len(m) for m in got])
    assert got == [text]


def test_large_message_to_player_is_split(player, observe):
    text = noise(10000)
    player.backend.text(text)
    step = player.collect()
    observe("frames", step.frames)
    heard = player.heard()
    observe("player_received", [len(m) for m in heard])
    assert heard == [text]


def test_unacknowledged_datagram_is_resent(player, observe):
    player.auto_ack = False
    player.backend.text("resend")
    first = player.collect(quiet=0.02)
    resent = player.collect(quiet=0.5, limit=0.5)
    observe("first", first.frames)
    observe("resent_same_message", [f.get("reliable_index") for f in resent.frames][:1])
    sequences = player.received_sequences[:]
    player.session.nak([sequences[-1]])
    after_nak = player.collect(quiet=0.05, limit=0.3)
    observe("resent_after_nak", [f.get("reliable_index") for f in after_nak.frames][:1])
    player.auto_ack = True
    player.session.ack(player.received_sequences)
    time.sleep(0.3)
    player.collect(quiet=0.1)
    observe("quiet_after_ack", player.collect(quiet=0.3, limit=0.6).summary())


def test_connected_ping_is_answered(player, observe):
    observe("pong", player.send(wire.connected_ping(1234), R.UNRELIABLE).summary())


def test_second_connection_request_is_accepted_again(player, observe):
    request = wire.connection_request(player.guid, 77)
    observe("step", player.send(request, R.RELIABLE).summary())


@pytest.mark.parametrize(
    "decode_logging, line",
    [
        (False, r"WARN Invalid packet received\. Turn on packet-decode-logging under \[debug\] in endgate\.toml"),
        (True, r"ERROR \d+\.\d+\.\d+\.\d+:\d+: invalid packet received: .+"),
    ],
)
def test_a_malformed_datagram_is_logged(make_proxy, backend, join_raw, decode_logging, line):
    proxy = make_proxy(backend.port, packet_decode_logging=decode_logging)
    client = join_raw(proxy)
    session = client.session
    batch = client.encode("probe")
    client.exchange(wire.data_datagram(session.datagram_sequence, [Frame(R.RELIABLE, batch * 20, 50)])[:-10])
    assert proxy.wait_for(line), proxy.lines[-20:]


@pytest.mark.parametrize(
    "name, datagram",
    [
        ("tiny", b"\x84\x00"),
        ("invalid_header", b"\x40\x00\x00\x00\x00"),
        ("ack_inverted_range", bytes([0xC0]) + wire.Writer().u16(1).u8(0).u24le(5).u24le(2).bytes()),
        ("ack_max_index", bytes([0xC0]) + wire.Writer().u16(1).u8(0).u24le(0).u24le(0xFFFFFF).bytes()),
        ("ack_too_many_ranges", bytes([0xC0]) + wire.encode_ranges([(i * 2, i * 2) for i in range(0x401)])),
        ("ack_too_many_numbers", bytes([0xC0]) + wire.encode_ranges([(0, 0x4000)])),
        ("ack_truncated", bytes([0xC0]) + wire.Writer().u16(3).u8(1).u24le(1).bytes()),
        ("nak_inverted_range", bytes([0xA0]) + wire.Writer().u16(1).u8(0).u24le(5).u24le(2).bytes()),
        ("nak_too_many_numbers", bytes([0xA0]) + wire.encode_ranges([(0, 0x4000)])),
        ("frame_channel_32", None),
        ("frame_zero_length", None),
        ("frame_split_index_past_count", None),
        ("frame_truncated_payload", None),
        ("sequence_jump", None),
        ("reliable_index_jump", None),
    ],
)
def test_malformed_datagrams_are_dropped(player, proxy, observe, name, datagram):
    session = player.session
    batch = player.encode("probe")
    if datagram is None:
        sequence = session.datagram_sequence
        if name == "frame_channel_32":
            datagram = wire.data_datagram(sequence, [Frame(R.RELIABLE_ORDERED, batch, 50, None, 0, 32)])
        elif name == "frame_zero_length":
            datagram = wire.data_datagram(sequence, [Frame(R.RELIABLE, b"", 50)])
        elif name == "frame_split_index_past_count":
            datagram = wire.data_datagram(sequence, [Frame(R.RELIABLE, batch, 50, split_count=2, split_index=2)])
        elif name == "frame_truncated_payload":
            datagram = wire.data_datagram(sequence, [Frame(R.RELIABLE, batch * 20, 50)])[:-10]
        elif name == "sequence_jump":
            datagram = wire.data_datagram(sequence + 60000, [Frame(R.UNRELIABLE, batch)])
        elif name == "reliable_index_jump":
            datagram = wire.data_datagram(sequence, [Frame(R.RELIABLE, batch, 1_500_000)])
        session.datagram_sequence += 1
    observe("step", player.exchange(datagram).summary())
    observe("backend_received", player.backend.texts(0.3))
    observe("logs", proxy.events("WARN|ERROR"))
    observe("still_forwarding", player.say("still forwarding").summary())
    observe("backend_after", player.backend.texts())


def test_player_disconnect_closes_the_backend(player, proxy, observe):
    observe("step", player.send(bytes([ID.DISCONNECTION_NOTIFICATION])).summary())
    observe("backend", outcome(player.backend, 3))
    observe("logs", [line.replace(str(player.address[1]), "<player>") for line in proxy.events()])


def test_backend_disconnect_closes_the_player(player, proxy, observe):
    player.backend.close()
    observe("step", player.collect(quiet=0.3).summary())
    time.sleep(0.2)
    observe("logs", [line.replace(str(player.address[1]), "<player>") for line in proxy.events()])


def test_unverified_sender_with_other_message_is_dropped(proxy, observe):
    client = RawClient(proxy.address)
    try:
        observe("open", client.open())
        observe("step", client.send(USER + b"\x0c", R.RELIABLE).summary())
        observe("reopen", client.exchange(wire.open_connection_request_2(proxy.address, 1400, client.guid)).summary())
    finally:
        client.close()
    observe("logs", proxy.events())


def test_wrong_password_is_rejected(proxy, observe):
    client = RawClient(proxy.address)
    try:
        observe("transcript", client.connect(password=b"secret"))
        observe("reopen", client.exchange(wire.open_connection_request_2(proxy.address, 1400, client.guid)).summary())
    finally:
        client.close()
    observe("logs", proxy.events())


def test_silent_player_times_out(player, proxy, observe):
    player.auto_ack = False
    player.auto_pong = False
    started = time.monotonic()
    observe("backend", outcome(player.backend, 20))
    observe("seconds", round(time.monotonic() - started))
    step = player.collect(quiet=0.05, limit=0.1)
    observe("logs", [line.replace(str(player.address[1]), "<player>") for line in proxy.events()])
    del step
