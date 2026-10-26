from __future__ import annotations

import ipaddress
import socket
import struct
import time
from dataclasses import dataclass, field
from enum import IntEnum

MAGIC = bytes.fromhex("00ffff00fefefefefdfdfdfd12345678")
PROTOCOL_VERSION = 11
UDP_HEADER_SIZE = 28
INTERNAL_ADDRESS_COUNT = 20


class ID(IntEnum):
    CONNECTED_PING = 0x00
    UNCONNECTED_PING = 0x01
    UNCONNECTED_PING_OPEN_CONNECTIONS = 0x02
    CONNECTED_PONG = 0x03
    DETECT_LOST_CONNECTIONS = 0x04
    OPEN_CONNECTION_REQUEST_1 = 0x05
    OPEN_CONNECTION_REPLY_1 = 0x06
    OPEN_CONNECTION_REQUEST_2 = 0x07
    OPEN_CONNECTION_REPLY_2 = 0x08
    CONNECTION_REQUEST = 0x09
    SND_RECEIPT_ACKED = 0x0E
    SND_RECEIPT_LOSS = 0x0F
    CONNECTION_REQUEST_ACCEPTED = 0x10
    CONNECTION_ATTEMPT_FAILED = 0x11
    ALREADY_CONNECTED = 0x12
    NEW_INCOMING_CONNECTION = 0x13
    NO_FREE_INCOMING_CONNECTIONS = 0x14
    DISCONNECTION_NOTIFICATION = 0x15
    CONNECTION_LOST = 0x16
    CONNECTION_BANNED = 0x17
    INVALID_PASSWORD = 0x18
    INCOMPATIBLE_PROTOCOL_VERSION = 0x19
    IP_RECENTLY_CONNECTED = 0x1A
    TIMESTAMP = 0x1B
    UNCONNECTED_PONG = 0x1C
    USER_PACKET_ENUM = 0x86


class Reliability(IntEnum):
    UNRELIABLE = 0
    UNRELIABLE_SEQUENCED = 1
    RELIABLE = 2
    RELIABLE_ORDERED = 3
    RELIABLE_SEQUENCED = 4
    UNRELIABLE_WITH_ACK_RECEIPT = 5
    RELIABLE_WITH_ACK_RECEIPT = 6
    RELIABLE_ORDERED_WITH_ACK_RECEIPT = 7


RELIABLE = {Reliability.RELIABLE, Reliability.RELIABLE_ORDERED, Reliability.RELIABLE_SEQUENCED}
SEQUENCED = {Reliability.UNRELIABLE_SEQUENCED, Reliability.RELIABLE_SEQUENCED}
ORDERED = SEQUENCED | {Reliability.RELIABLE_ORDERED, Reliability.RELIABLE_ORDERED_WITH_ACK_RECEIPT}


class Reader:
    def __init__(self, data: bytes) -> None:
        self.data = data
        self.offset = 0

    def remaining(self) -> int:
        return len(self.data) - self.offset

    def take(self, size: int) -> bytes:
        if self.offset + size > len(self.data):
            raise ValueError(f"read {size} bytes at {self.offset} past end {len(self.data)}")
        chunk = self.data[self.offset : self.offset + size]
        self.offset += size
        return chunk

    def u8(self) -> int:
        return self.take(1)[0]

    def u16(self) -> int:
        return struct.unpack(">H", self.take(2))[0]

    def u24le(self) -> int:
        return int.from_bytes(self.take(3), "little")

    def u32(self) -> int:
        return struct.unpack(">I", self.take(4))[0]

    def u64(self) -> int:
        return struct.unpack(">Q", self.take(8))[0]

    def f32(self) -> float:
        return struct.unpack(">f", self.take(4))[0]

    def address(self) -> tuple[str, int]:
        version = self.u8()
        if version == 4:
            raw = bytes(b ^ 0xFF for b in self.take(4))
            port = self.u16()
            return str(ipaddress.IPv4Address(raw)), port
        family, port, _flow = struct.unpack("<HHI", self.take(8))
        raw = self.take(16)
        self.take(4)
        return str(ipaddress.IPv6Address(raw)), socket.ntohs(port)

    def rest(self) -> bytes:
        return self.take(self.remaining())


class Writer:
    def __init__(self) -> None:
        self.parts: list[bytes] = []

    def raw(self, data: bytes) -> Writer:
        self.parts.append(bytes(data))
        return self

    def u8(self, value: int) -> Writer:
        return self.raw(bytes([value & 0xFF]))

    def u16(self, value: int) -> Writer:
        return self.raw(struct.pack(">H", value & 0xFFFF))

    def u24le(self, value: int) -> Writer:
        return self.raw((value & 0xFFFFFF).to_bytes(3, "little"))

    def u32(self, value: int) -> Writer:
        return self.raw(struct.pack(">I", value & 0xFFFFFFFF))

    def u64(self, value: int) -> Writer:
        return self.raw(struct.pack(">Q", value & 0xFFFFFFFFFFFFFFFF))

    def address(self, address: tuple[str, int]) -> Writer:
        host, port = address
        packed = ipaddress.IPv4Address(host).packed
        return self.u8(4).raw(bytes(b ^ 0xFF for b in packed)).u16(port)

    def bytes(self) -> bytes:
        return b"".join(self.parts)


@dataclass
class Frame:
    reliability: int
    payload: bytes
    reliable_index: int | None = None
    sequence_index: int | None = None
    order_index: int | None = None
    channel: int | None = None
    split_count: int = 0
    split_id: int = 0
    split_index: int = 0

    @property
    def is_split(self) -> bool:
        return self.split_count > 0

    def encode(self) -> bytes:
        w = Writer()
        w.u8((self.reliability << 5) | (0x10 if self.is_split else 0))
        w.u16(len(self.payload) * 8)
        if self.reliability in RELIABLE or self.reliability in (
            Reliability.RELIABLE_WITH_ACK_RECEIPT,
            Reliability.RELIABLE_ORDERED_WITH_ACK_RECEIPT,
        ):
            w.u24le(self.reliable_index or 0)
        if self.reliability in SEQUENCED:
            w.u24le(self.sequence_index or 0)
        if self.reliability in ORDERED:
            w.u24le(self.order_index or 0)
            w.u8(self.channel or 0)
        if self.is_split:
            w.u32(self.split_count).u16(self.split_id).u32(self.split_index)
        w.raw(self.payload)
        return w.bytes()

    @staticmethod
    def decode(r: Reader) -> Frame:
        flags = r.u8()
        reliability = flags >> 5
        split = bool(flags & 0x10)
        bits = r.u16()
        frame = Frame(reliability, b"")
        if reliability in RELIABLE:
            frame.reliable_index = r.u24le()
        if reliability in SEQUENCED:
            frame.sequence_index = r.u24le()
        if reliability in ORDERED:
            frame.order_index = r.u24le()
            frame.channel = r.u8()
        if split:
            frame.split_count = r.u32()
            frame.split_id = r.u16()
            frame.split_index = r.u32()
        frame.payload = r.take((bits + 7) // 8)
        return frame


def encode_ranges(numbers: list[tuple[int, int]]) -> bytes:
    w = Writer().u16(len(numbers))
    for low, high in numbers:
        w.u8(1 if low == high else 0).u24le(low)
        if low != high:
            w.u24le(high)
    return w.bytes()


def decode_ranges(r: Reader) -> list[tuple[int, int]]:
    ranges = []
    for _ in range(r.u16()):
        single = r.u8()
        low = r.u24le()
        high = low if single else r.u24le()
        ranges.append((low, high))
    return ranges


def collapse(numbers: list[int]) -> list[tuple[int, int]]:
    ranges: list[tuple[int, int]] = []
    for n in sorted(set(numbers)):
        if ranges and ranges[-1][1] + 1 == n:
            ranges[-1] = (ranges[-1][0], n)
        else:
            ranges.append((n, n))
    return ranges


@dataclass
class Datagram:
    kind: str
    sequence: int | None = None
    flags: int = 0
    ranges: list[tuple[int, int]] = field(default_factory=list)
    frames: list[Frame] = field(default_factory=list)
    arrival_rate: float | None = None

    @staticmethod
    def decode(data: bytes) -> Datagram:
        r = Reader(data)
        head = r.u8()
        if not head & 0x80:
            return Datagram("offline", flags=head)
        if head & 0x40:
            rate = r.f32() if head & 0x20 else None
            return Datagram("ack", flags=head, ranges=decode_ranges(r), arrival_rate=rate)
        if head & 0x20:
            return Datagram("nak", flags=head, ranges=decode_ranges(r))
        datagram = Datagram("data", sequence=r.u24le(), flags=head)
        while r.remaining() >= 3:
            datagram.frames.append(Frame.decode(r))
        return datagram


def data_datagram(sequence: int, frames: list[Frame], flags: int = 0x84) -> bytes:
    return bytes([flags]) + (sequence & 0xFFFFFF).to_bytes(3, "little") + b"".join(f.encode() for f in frames)


def ack_datagram(numbers: list[int]) -> bytes:
    return bytes([0xC0]) + encode_ranges(collapse(numbers))


def nak_datagram(numbers: list[int]) -> bytes:
    return bytes([0xA0]) + encode_ranges(collapse(numbers))


def open_connection_request_1(mtu: int, protocol: int = PROTOCOL_VERSION) -> bytes:
    head = bytes([ID.OPEN_CONNECTION_REQUEST_1]) + MAGIC + bytes([protocol])
    return head + bytes(max(0, mtu - UDP_HEADER_SIZE - len(head)))


def open_connection_request_2(server: tuple[str, int], mtu: int, guid: int) -> bytes:
    return Writer().u8(ID.OPEN_CONNECTION_REQUEST_2).raw(MAGIC).address(server).u16(mtu).u64(guid).bytes()


def unconnected_ping(sent: int, guid: int, kind: int = ID.UNCONNECTED_PING) -> bytes:
    return Writer().u8(kind).u64(sent).raw(MAGIC).u64(guid).bytes()


def describe_offline(data: bytes) -> dict:
    """Decode an unconnected message, keeping only what is stable from run to run."""
    r = Reader(data)
    mid = r.u8()
    out: dict = {"id": ID(mid).name if mid in ID._value2member_map_ else mid, "length": len(data)}
    try:
        if mid == ID.UNCONNECTED_PONG:
            out["time"] = r.u64()
            r.u64()
            out["magic"] = r.take(16) == MAGIC
            out["payload"] = r.rest()
        elif mid == ID.OPEN_CONNECTION_REPLY_1:
            out["magic"] = r.take(16) == MAGIC
            r.u64()
            out["security"] = r.u8()
            out["mtu"] = r.u16()
        elif mid == ID.OPEN_CONNECTION_REPLY_2:
            out["magic"] = r.take(16) == MAGIC
            r.u64()
            out["address"] = r.address()
            out["mtu"] = r.u16()
            out["security"] = r.u8()
        elif mid == ID.OPEN_CONNECTION_REQUEST_1:
            out["magic"] = r.take(16) == MAGIC
            out["protocol"] = r.u8()
            out["padding_zero"] = not any(r.rest())
        elif mid == ID.OPEN_CONNECTION_REQUEST_2:
            out["magic"] = r.take(16) == MAGIC
            out["address"] = r.address()
            out["mtu"] = r.u16()
            r.u64()
        elif mid == ID.INCOMPATIBLE_PROTOCOL_VERSION:
            out["protocol"] = r.u8()
            out["magic"] = r.take(16) == MAGIC
            r.u64()
        elif mid in (ID.ALREADY_CONNECTED, ID.NO_FREE_INCOMING_CONNECTIONS, ID.IP_RECENTLY_CONNECTED):
            out["magic"] = r.take(16) == MAGIC
            r.u64()
        out["trailing"] = r.remaining()
    except ValueError as error:
        out["error"] = str(error)
    return out


def guid_of(data: bytes) -> int | None:
    mid = data[0]
    if mid == ID.UNCONNECTED_PONG:
        return Reader(data[9:]).u64()
    if mid in (ID.OPEN_CONNECTION_REPLY_1, ID.OPEN_CONNECTION_REPLY_2):
        return Reader(data[17:]).u64()
    return None


def describe_message(payload: bytes) -> dict:
    """Decode a message carried in a frame, masking clocks."""
    r = Reader(payload)
    mid = r.u8()
    out: dict = {"id": ID(mid).name if mid in ID._value2member_map_ else mid, "length": len(payload)}
    try:
        if mid == ID.CONNECTION_REQUEST_ACCEPTED:
            out["address"] = r.address()
            out["system_index"] = r.u16()
            out["internal"] = sorted({r.address() for _ in range(INTERNAL_ADDRESS_COUNT)})
            r.u64()
            r.u64()
        elif mid == ID.NEW_INCOMING_CONNECTION:
            out["address"] = r.address()
            out["internal"] = sorted({r.address() for _ in range(INTERNAL_ADDRESS_COUNT)})
            r.u64()
            r.u64()
        elif mid == ID.CONNECTION_REQUEST:
            r.u64()
            r.u64()
            out["security"] = r.u8()
            out["password"] = r.rest().hex()
        elif mid == ID.CONNECTED_PING:
            r.u64()
        elif mid == ID.CONNECTED_PONG:
            r.u64()
            r.u64()
        elif mid >= ID.USER_PACKET_ENUM:
            out["payload"] = payload.hex() if len(payload) <= 64 else f"{len(payload)}:{hash(payload) & 0xFFFFFFFF:08x}"
            r.rest()
        out["trailing"] = r.remaining()
    except ValueError as error:
        out["error"] = str(error)
    return out


def describe_frame(frame: Frame) -> dict:
    out = {"reliability": Reliability(frame.reliability).name}
    for key in ("reliable_index", "sequence_index", "order_index", "channel"):
        value = getattr(frame, key)
        if value is not None:
            out[key] = value
    if frame.is_split:
        out["split"] = [frame.split_count, frame.split_id, frame.split_index, len(frame.payload)]
    else:
        out["message"] = describe_message(frame.payload)
    return out


def describe_datagram(data: bytes) -> dict:
    datagram = Datagram.decode(data)
    if datagram.kind == "offline":
        return {"kind": "offline", **describe_offline(data)}
    out: dict = {"kind": datagram.kind, "head": f"{datagram.flags:02x}", "length": len(data)}
    if datagram.kind in ("ack", "nak"):
        out["ranges"] = datagram.ranges
        if datagram.arrival_rate is not None:
            out["has_arrival_rate"] = True
        return out
    out["sequence"] = datagram.sequence
    out["frames"] = [describe_frame(f) for f in datagram.frames]
    return out


class Socket:
    """A UDP socket that records every datagram it receives."""

    def __init__(self, bind: tuple[str, int] = ("127.0.0.1", 0)) -> None:
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
        self.sock.bind(bind)

    @property
    def address(self) -> tuple[str, int]:
        return self.sock.getsockname()

    def send(self, data: bytes, target: tuple[str, int]) -> None:
        self.sock.sendto(data, target)

    def recv(self, timeout: float) -> tuple[bytes, tuple[str, int]] | None:
        self.sock.settimeout(max(timeout, 0.0001))
        try:
            return self.sock.recvfrom(65535)
        except (TimeoutError, socket.timeout):
            return None

    def drain(self, quiet: float = 0.15, limit: float = 3.0) -> list[tuple[bytes, tuple[str, int]]]:
        """Collect datagrams until nothing arrives for `quiet` seconds."""
        received = []
        deadline = time.monotonic() + limit
        while time.monotonic() < deadline:
            item = self.recv(quiet)
            if item is None:
                break
            received.append(item)
        return received

    def close(self) -> None:
        self.sock.close()


class Session:
    """The sending half of a RakNet connection, with every counter under the test's control."""

    def __init__(self, sock: Socket, peer: tuple[str, int], mtu: int) -> None:
        self.sock = sock
        self.peer = peer
        self.mtu = mtu
        self.datagram_sequence = 0
        self.reliable_index = 0
        self.order_index = [0] * 32
        self.sequence_index = [0] * 32
        self.split_id = 0

    def _order(self, frame: Frame, channel: int) -> None:
        if frame.reliability in SEQUENCED:
            frame.order_index = self.order_index[channel]
            frame.sequence_index = self.sequence_index[channel]
            self.sequence_index[channel] += 1
            frame.channel = channel
        elif frame.reliability in ORDERED:
            frame.order_index = self.order_index[channel]
            self.order_index[channel] += 1
            self.sequence_index[channel] = 0
            frame.channel = channel

    def _number(self, frame: Frame) -> None:
        if frame.reliability in RELIABLE:
            frame.reliable_index = self.reliable_index
            self.reliable_index += 1

    def frame(self, payload: bytes, reliability: int = Reliability.RELIABLE_ORDERED, channel: int = 0) -> Frame:
        frame = Frame(reliability, payload)
        self._number(frame)
        self._order(frame, channel)
        return frame

    def split(
        self, payload: bytes, chunk: int, reliability: int = Reliability.RELIABLE_ORDERED, channel: int = 0
    ) -> list[Frame]:
        template = Frame(reliability, b"")
        self._order(template, channel)
        count = (len(payload) + chunk - 1) // chunk
        frames = []
        for index in range(count):
            part = Frame(
                reliability,
                payload[index * chunk : (index + 1) * chunk],
                sequence_index=template.sequence_index,
                order_index=template.order_index,
                channel=template.channel,
                split_count=count,
                split_id=self.split_id,
                split_index=index,
            )
            self._number(part)
            frames.append(part)
        self.split_id = (self.split_id + 1) & 0xFFFF
        return frames

    def send_frames(self, frames: list[Frame], sequence: int | None = None, flags: int = 0x84) -> int:
        if sequence is None:
            sequence = self.datagram_sequence
            self.datagram_sequence += 1
        self.sock.send(data_datagram(sequence, frames, flags), self.peer)
        return sequence

    def send(self, payload: bytes, reliability: int = Reliability.RELIABLE_ORDERED, channel: int = 0) -> int:
        return self.send_frames([self.frame(payload, reliability, channel)])

    def ack(self, numbers: list[int]) -> None:
        if numbers:
            self.sock.send(ack_datagram(numbers), self.peer)

    def nak(self, numbers: list[int]) -> None:
        if numbers:
            self.sock.send(nak_datagram(numbers), self.peer)


def connection_request(guid: int, sent: int, password: bytes = b"") -> bytes:
    return Writer().u8(ID.CONNECTION_REQUEST).u64(guid).u64(sent).u8(0).raw(password).bytes()


def new_incoming_connection(server: tuple[str, int], pong_time: int, now: int) -> bytes:
    w = Writer().u8(ID.NEW_INCOMING_CONNECTION).address(server)
    for _ in range(INTERNAL_ADDRESS_COUNT):
        w.address(("0.0.0.0", 0))
    return w.u64(pong_time).u64(now).bytes()


def connection_request_accepted(client: tuple[str, int], request_time: int, now: int) -> bytes:
    w = Writer().u8(ID.CONNECTION_REQUEST_ACCEPTED).address(client).u16(0)
    for _ in range(INTERNAL_ADDRESS_COUNT):
        w.address(("0.0.0.0", 0))
    return w.u64(request_time).u64(now).bytes()


def connected_pong(ping_time: int, now: int) -> bytes:
    return Writer().u8(ID.CONNECTED_PONG).u64(ping_time).u64(now).bytes()


def connected_ping(now: int) -> bytes:
    return Writer().u8(ID.CONNECTED_PING).u64(now).bytes()


def open_connection_reply_1(guid: int, mtu: int) -> bytes:
    return Writer().u8(ID.OPEN_CONNECTION_REPLY_1).raw(MAGIC).u64(guid).u8(0).u16(mtu).bytes()


def open_connection_reply_2(guid: int, client: tuple[str, int], mtu: int) -> bytes:
    return Writer().u8(ID.OPEN_CONNECTION_REPLY_2).raw(MAGIC).u64(guid).address(client).u16(mtu).u8(0).bytes()
