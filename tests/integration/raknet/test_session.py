import time

import pytest
import raknet
import wire
from raw import RawClient
from wire import ID, Frame
from wire import Reliability as R

USER = bytes([0xFE])


@pytest.fixture
def player(proxy, backend):
    client = RawClient(proxy.address)
    client.transcript = client.connect()
    client.backend = backend.accept(timeout=5)
    time.sleep(0.05)
    yield client
    client.close()


def received(connection, timeout: float = 0.5) -> list[bytes]:
    messages = []
    while True:
        try:
            messages.append(connection.recv(timeout=timeout))
        except (TimeoutError, raknet.ConnectionClosed):
            return messages
        timeout = 0.05


def test_handshake(player, proxy, observe):
    observe("transcript", player.transcript)
    observe("logs", [line.replace(str(player.address[1]), "<player>") for line in proxy.events()])


def test_message_round_trip(player, observe):
    observe("to_backend", player.send(USER + b"hello").summary())
    observe("backend_received", [m.hex() for m in received(player.backend)])
    player.backend.send(USER + b"world")
    observe("to_player", player.collect().summary())
    observe("player_received", [m.hex() for m in player.messages])


@pytest.mark.parametrize("reliability", list(R))
def test_each_reliability_is_forwarded(player, observe, reliability):
    observe("step", player.send(USER + bytes([reliability]), reliability).summary())
    observe("backend_received", [m.hex() for m in received(player.backend)])


def test_ordered_messages_wait_for_the_gap(player, observe):
    session = player.session
    first = session.frame(USER + b"\x00", R.RELIABLE_ORDERED, 3)
    second = session.frame(USER + b"\x01", R.RELIABLE_ORDERED, 3)
    observe("second_first", player.exchange(wire.data_datagram(session.datagram_sequence + 1, [second])).summary())
    observe("held_back", [m.hex() for m in received(player.backend, 0.3)])
    observe("gap_filled", player.exchange(wire.data_datagram(session.datagram_sequence, [first])).summary())
    session.datagram_sequence += 2
    observe("released", [m.hex() for m in received(player.backend)])


def test_sequenced_messages_drop_older(player, observe):
    session = player.session
    frames = [session.frame(USER + bytes([i]), R.UNRELIABLE_SEQUENCED, 5) for i in range(3)]
    for frame in (frames[2], frames[0], frames[1]):
        step = player.exchange(wire.data_datagram(session.datagram_sequence, [frame]))
        observe(f"sent_{frame.sequence_index}", step.summary())
        session.datagram_sequence += 1
    observe("backend_received", [m.hex() for m in received(player.backend)])


def test_skipped_datagrams_are_naked(player, observe):
    session = player.session
    session.datagram_sequence += 3
    observe("jump", player.send(USER + b"\x07").summary())
    observe("backend_received", [m.hex() for m in received(player.backend)])


def test_duplicate_datagram_is_acked_and_dropped(player, observe):
    session = player.session
    datagram = wire.data_datagram(session.datagram_sequence, [session.frame(USER + b"\x08", R.RELIABLE_ORDERED)])
    session.datagram_sequence += 1
    observe("first", player.exchange(datagram).summary())
    observe("again", player.exchange(datagram).summary())
    observe("backend_received", [m.hex() for m in received(player.backend)])


def test_split_message_is_reassembled_out_of_order(player, observe):
    session = player.session
    payload = USER + bytes(range(256)) * 12
    parts = session.split(payload, 500)
    steps = []
    for part in (parts[3], parts[0], parts[5], parts[1], parts[4], parts[2], parts[6]):
        steps.append(player.exchange(wire.data_datagram(session.datagram_sequence, [part])).summary())
        session.datagram_sequence += 1
    observe("steps", steps)
    got = received(player.backend)
    observe("backend_received", [len(m) for m in got])
    assert got == [payload]


def test_large_message_to_player_is_split(player, observe):
    payload = USER + bytes(range(256)) * 40
    player.backend.send(payload)
    step = player.collect()
    observe("frames", step.frames)
    observe("player_received", [len(m) for m in player.messages])
    assert player.messages == [payload]


def test_unacknowledged_datagram_is_resent(player, observe):
    player.auto_ack = False
    player.backend.send(USER + b"\x09")
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
def test_a_malformed_datagram_is_logged(make_proxy, backend, decode_logging, line):
    proxy = make_proxy(backend.local_address[1], packet_decode_logging=decode_logging)
    client = RawClient(proxy.address)
    client.connect()
    backend.accept(timeout=5)
    session = client.session
    client.exchange(wire.data_datagram(session.datagram_sequence, [Frame(R.RELIABLE, USER * 20, 50)])[:-10])
    assert proxy.wait_for(line), proxy.lines[-20:]
    client.close()


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
    if datagram is None:
        sequence = session.datagram_sequence
        if name == "frame_channel_32":
            datagram = wire.data_datagram(sequence, [Frame(R.RELIABLE_ORDERED, USER, 50, None, 0, 32)])
        elif name == "frame_zero_length":
            datagram = wire.data_datagram(sequence, [Frame(R.RELIABLE, b"", 50)])
        elif name == "frame_split_index_past_count":
            datagram = wire.data_datagram(sequence, [Frame(R.RELIABLE, USER, 50, split_count=2, split_index=2)])
        elif name == "frame_truncated_payload":
            datagram = wire.data_datagram(sequence, [Frame(R.RELIABLE, USER * 20, 50)])[:-10]
        elif name == "sequence_jump":
            datagram = wire.data_datagram(sequence + 60000, [Frame(R.UNRELIABLE, USER)])
        elif name == "reliable_index_jump":
            datagram = wire.data_datagram(sequence, [Frame(R.RELIABLE, USER + b"\x0a", 1_500_000)])
        session.datagram_sequence += 1
    observe("step", player.exchange(datagram).summary())
    observe("backend_received", [m.hex() for m in received(player.backend, 0.3)])
    observe("logs", proxy.events("WARN|ERROR"))
    observe("still_forwarding", player.send(USER + b"\x0b").summary())
    observe("backend_after", [m.hex() for m in received(player.backend)])


def test_player_disconnect_closes_the_backend(player, proxy, observe):
    observe("step", player.send(bytes([ID.DISCONNECTION_NOTIFICATION])).summary())
    try:
        player.backend.recv(timeout=3)
        outcome = "message"
    except raknet.ConnectionClosedOK:
        outcome = "closed_ok"
    except raknet.ConnectionClosedError:
        outcome = "closed_error"
    except TimeoutError:
        outcome = "timeout"
    observe("backend", outcome)
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
    try:
        player.backend.recv(timeout=20)
        outcome = "message"
    except raknet.ConnectionClosedOK:
        outcome = "closed_ok"
    except raknet.ConnectionClosedError:
        outcome = "closed_error"
    except TimeoutError:
        outcome = "timeout"
    observe("backend", outcome)
    observe("seconds", round(time.monotonic() - started))
    step = player.collect(quiet=0.05, limit=0.1)
    observe("logs", [line.replace(str(player.address[1]), "<player>") for line in proxy.events()])
    del step
