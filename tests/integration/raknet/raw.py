from __future__ import annotations

import random
import time
from dataclasses import dataclass, field

import wire
from wire import ID, Datagram, Frame, Reliability, Session, Socket


def millis() -> int:
    return int(time.monotonic() * 1000)


@dataclass
class Step:
    """What a peer sent back within one exchange, normalised so two runs of the same build compare equal."""

    acks: list[int] = field(default_factory=list)
    naks: list[int] = field(default_factory=list)
    frames: list[dict] = field(default_factory=list)
    offline: list[dict] = field(default_factory=list)
    datagrams: int = 0

    def summary(self) -> dict:
        out: dict = {}
        if self.acks:
            out["acks"] = wire.collapse(self.acks)
        if self.naks:
            out["naks"] = wire.collapse(self.naks)
        if self.frames:
            out["frames"] = self.frames
        if self.offline:
            out["offline"] = self.offline
        return out


class Assembler:
    def __init__(self) -> None:
        self.parts: dict[int, dict[int, bytes]] = {}

    def add(self, frame: Frame) -> bytes | None:
        if not frame.is_split:
            return frame.payload
        parts = self.parts.setdefault(frame.split_id, {})
        parts[frame.split_index] = frame.payload
        if len(parts) < frame.split_count:
            return None
        del self.parts[frame.split_id]
        return b"".join(parts[i] for i in range(frame.split_count))


class RawPeer:
    """One end of a RakNet connection written out by hand, so the test controls every byte the proxy sees."""

    def __init__(self, sock: Socket | None = None, guid: int | None = None) -> None:
        self.sock = sock or Socket()
        self.guid = guid if guid is not None else random.getrandbits(63)
        self.remote: tuple[str, int] | None = None
        self.session: Session | None = None
        self.assembler = Assembler()
        self.messages: list[bytes] = []
        self.received_sequences: list[int] = []
        self.auto_ack = True
        self.auto_pong = True
        self.last_accepted: bytes | None = None
        self.last_request: bytes | None = None

    @property
    def address(self) -> tuple[str, int]:
        return self.sock.address

    def _canonical_frame(self, frame: Frame) -> dict:
        described = wire.describe_frame(frame)
        message = described.get("message")
        if message and message.get("address") == self.address:
            message["address"] = "self"
        return described

    def handle(self, data: bytes, step: Step) -> None:
        datagram = Datagram.decode(data)
        step.datagrams += 1
        if datagram.kind == "offline":
            step.offline.append(wire.describe_offline(data))
            return
        if datagram.kind == "ack":
            for low, high in datagram.ranges:
                step.acks.extend(range(low, high + 1))
            return
        if datagram.kind == "nak":
            for low, high in datagram.ranges:
                step.naks.extend(range(low, high + 1))
            return
        self.received_sequences.append(datagram.sequence)
        if self.auto_ack and self.session is not None:
            self.session.ack([datagram.sequence])
        for frame in datagram.frames:
            step.frames.append(self._canonical_frame(frame))
            message = self.assembler.add(frame)
            if message is None:
                continue
            self.on_message(message)

    def on_message(self, message: bytes) -> None:
        mid = message[0]
        if mid == ID.CONNECTED_PING and self.auto_pong and self.session is not None:
            ping_time = wire.Reader(message[1:9]).u64()
            self.session.send(wire.connected_pong(ping_time, millis()), Reliability.UNRELIABLE)
        elif mid == ID.CONNECTION_REQUEST_ACCEPTED:
            self.last_accepted = message
        elif mid == ID.CONNECTION_REQUEST:
            self.last_request = message
        elif mid >= ID.TIMESTAMP:
            self.messages.append(message)

    def collect(self, quiet: float = 0.15, limit: float = 5.0) -> Step:
        step = Step()
        deadline = time.monotonic() + limit
        while time.monotonic() < deadline:
            item = self.sock.recv(quiet)
            if item is None:
                break
            data, source = item
            if self.remote is None or source == self.remote:
                self.handle(data, step)
        step.acks.sort()
        step.naks.sort()
        return step

    def exchange(self, *datagrams: bytes, quiet: float = 0.15) -> Step:
        assert self.remote is not None
        for data in datagrams:
            self.sock.send(data, self.remote)
        return self.collect(quiet)

    def close(self) -> None:
        self.sock.close()


class RawClient(RawPeer):
    def __init__(self, server: tuple[str, int], mtu: int = 1400, guid: int | None = None) -> None:
        super().__init__(guid=guid)
        self.remote = server
        self.mtu = mtu

    def open(self) -> list[dict]:
        transcript = []
        reply1 = self.exchange(wire.open_connection_request_1(self.mtu))
        transcript.append(reply1.summary())
        reply2 = self.exchange(wire.open_connection_request_2(self.remote, self.mtu, self.guid))
        summary = reply2.summary()
        for item in summary.get("offline", []):
            if item.get("address") == self.address:
                item["address"] = "self"
        transcript.append(summary)
        self.session = Session(self.sock, self.remote, self.mtu)
        return transcript

    def connect(self, password: bytes = b"") -> list[dict]:
        transcript = self.open()
        assert self.session is not None
        request = self.session.frame(wire.connection_request(self.guid, millis(), password), Reliability.RELIABLE)
        transcript.append(self.exchange(wire.data_datagram(self.session.datagram_sequence, [request])).summary())
        self.session.datagram_sequence += 1
        if self.last_accepted is None:
            return transcript
        server_time = wire.Reader(self.last_accepted[-8:]).u64()
        hello = wire.new_incoming_connection(self.remote, server_time, millis())
        transcript.append(self.exchange(self._datagram(hello, Reliability.RELIABLE_ORDERED)).summary())
        return transcript

    def _datagram(self, payload: bytes, reliability: int, channel: int = 0) -> bytes:
        assert self.session is not None
        frame = self.session.frame(payload, reliability, channel)
        sequence = self.session.datagram_sequence
        self.session.datagram_sequence += 1
        return wire.data_datagram(sequence, [frame])

    def send(self, payload: bytes, reliability: int = Reliability.RELIABLE_ORDERED, channel: int = 0) -> Step:
        return self.exchange(self._datagram(payload, reliability, channel))


class RawServer(RawPeer):
    """A backend that answers the handshake itself, so the proxy's client half can be recorded."""

    def __init__(self, mtu_limit: int = 1600, respond: bool = True) -> None:
        super().__init__()
        self.mtu_limit = mtu_limit
        self.respond = respond
        self.transcript: list[dict] = []
        self.request_sizes: list[int] = []
        self.request_times: list[float] = []

    def serve(self, until: float, quiet: float = 0.15) -> None:
        """Answer the proxy until `until` (monotonic) has passed."""
        while time.monotonic() < until:
            item = self.sock.recv(min(quiet, max(until - time.monotonic(), 0.001)))
            if item is None:
                continue
            data, source = item
            if self.remote is None:
                self.remote = source
            step = Step()
            if data[0] == ID.OPEN_CONNECTION_REQUEST_1:
                self.request_sizes.append(len(data))
                self.request_times.append(time.monotonic())
                step.offline.append(wire.describe_offline(data))
                if self.respond:
                    self.sock.send(wire.open_connection_reply_1(self.guid, min(len(data) + 28, self.mtu_limit)), source)
            elif data[0] == ID.OPEN_CONNECTION_REQUEST_2:
                described = wire.describe_offline(data)
                if described.get("address") == self.address:
                    described["address"] = "self"
                step.offline.append(described)
                request = wire.Reader(data[17:])
                request.address()
                mtu = request.u16()
                self.session = Session(self.sock, source, mtu)
                self.sock.send(wire.open_connection_reply_2(self.guid, source, mtu), source)
            else:
                self.handle(data, step)
                if self.last_request is not None and self.session is not None:
                    request_time = wire.Reader(self.last_request[9:17]).u64()
                    accepted = wire.connection_request_accepted(source, request_time, millis())
                    self.session.send(accepted, Reliability.RELIABLE_ORDERED)
                    self.last_request = None
            summary = step.summary()
            if summary:
                self.transcript.append(summary)
