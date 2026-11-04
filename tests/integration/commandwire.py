"""AvailableCommandsPacket's body as a dict, written and read the way packet.md describes the wire."""

from __future__ import annotations

import struct

PERMISSIONS = ("any", "gamedirectors", "admin", "host", "owner", "internal")
ENUM_SYMBOL = 0x300000
SOFT_ENUM_SYMBOL = 0x4100000
ID = 0x100038
MESSAGE_ROOT = 0x100044


def empty() -> dict:
    return {
        "enum_values": [],
        "chained_subcommand_values": [],
        "postfixes": [],
        "enums": [],
        "chained_subcommands": [],
        "commands": [],
        "soft_enums": [],
        "constraints": [],
    }


def command(name: str, *overloads: list[dict], flags: int = 0, permission: str = "any", alias_enum: int = -1) -> dict:
    return {
        "name": name,
        "description": f"commands.{name}.description",
        "flags": flags,
        "permission": permission,
        "alias_enum": alias_enum,
        "chained_subcommand_indexes": [],
        "overloads": [{"is_chaining": False, "params": params} for params in (overloads or ([],))],
    }


def param(name: str, parse_symbol: int, optional: bool = False, options: int = 0) -> dict:
    return {"name": name, "parse_symbol": parse_symbol, "optional": optional, "param_options": options}


def backend(names: tuple[str, ...] = ("help", "list", "say")) -> dict:
    """A small backend registry: commands with a word, a message and a keyword enum, plus a soft enum."""
    packet = empty()
    packet["enum_values"] = ["true", "false", "day", "night", *names]
    packet["enums"] = [
        {"name": "Boolean", "values": [0, 1]},
        {"name": "TimeSpec", "values": [2, 3]},
        {"name": "CommandName", "values": list(range(4, 4 + len(names)))},
    ]
    packet["soft_enums"] = [{"name": "TagValues", "values": ["a", "b"]}]
    for name in names:
        if name == "say":
            packet["commands"].append(command(name, [param("message", MESSAGE_ROOT)], flags=192))
        else:
            packet["commands"].append(command(name, [param("mode", ENUM_SYMBOL | 1, True)]))
    return packet


class Writer:
    def __init__(self) -> None:
        self.out = bytearray()

    def varint(self, value: int) -> None:
        while True:
            byte = value & 0x7F
            value >>= 7
            if value:
                self.out.append(byte | 0x80)
            else:
                self.out.append(byte)
                return

    def string(self, text: str) -> None:
        data = text.encode()
        self.varint(len(data))
        self.out.extend(data)

    def u32(self, value: int) -> None:
        self.out.extend(struct.pack("<I", value))


def encode(packet: dict) -> bytes:
    writer = Writer()
    for key in ("enum_values", "chained_subcommand_values", "postfixes"):
        writer.varint(len(packet[key]))
        for text in packet[key]:
            writer.string(text)
    writer.varint(len(packet["enums"]))
    for enum in packet["enums"]:
        writer.string(enum["name"])
        writer.varint(len(enum["values"]))
        for value in enum["values"]:
            writer.u32(value)
    writer.varint(len(packet["chained_subcommands"]))
    for chained in packet["chained_subcommands"]:
        writer.string(chained["name"])
        writer.varint(len(chained["values"]))
        for pair in chained["values"]:
            writer.varint(pair["sub_command_first_value"])
            writer.varint(pair["sub_command_second_value"])
    writer.varint(len(packet["commands"]))
    for item in packet["commands"]:
        writer.string(item["name"])
        writer.string(item["description"])
        writer.out.extend(struct.pack("<H", item["flags"]))
        writer.string(item["permission"])
        writer.out.extend(struct.pack("<i", item["alias_enum"]))
        writer.varint(len(item["chained_subcommand_indexes"]))
        for index in item["chained_subcommand_indexes"]:
            writer.u32(index)
        writer.varint(len(item["overloads"]))
        for overload in item["overloads"]:
            writer.out.append(1 if overload["is_chaining"] else 0)
            writer.varint(len(overload["params"]))
            for parameter in overload["params"]:
                writer.string(parameter["name"])
                writer.u32(parameter["parse_symbol"])
                writer.out.append(1 if parameter["optional"] else 0)
                writer.out.append(parameter["param_options"])
    writer.varint(len(packet["soft_enums"]))
    for soft in packet["soft_enums"]:
        writer.string(soft["name"])
        writer.varint(len(soft["values"]))
        for value in soft["values"]:
            writer.string(value)
    writer.varint(len(packet["constraints"]))
    for constraint in packet["constraints"]:
        writer.u32(constraint["enum_value_symbol"])
        writer.u32(constraint["enum_symbol"])
        writer.varint(len(constraint["constraints"]))
        writer.out.extend(constraint["constraints"])
    return bytes(writer.out)


class Reader:
    def __init__(self, data: bytes) -> None:
        self.data = data
        self.at = 0

    def varint(self) -> int:
        result = shift = 0
        while True:
            byte = self.data[self.at]
            self.at += 1
            result |= (byte & 0x7F) << shift
            if not byte & 0x80:
                return result
            shift += 7

    def string(self) -> str:
        size = self.varint()
        text = self.data[self.at : self.at + size].decode()
        self.at += size
        return text

    def u32(self) -> int:
        (value,) = struct.unpack_from("<I", self.data, self.at)
        self.at += 4
        return value

    def take(self, fmt: str):
        (value,) = struct.unpack_from(fmt, self.data, self.at)
        self.at += struct.calcsize(fmt)
        return value

    def byte(self) -> int:
        self.at += 1
        return self.data[self.at - 1]


def decode(data: bytes) -> dict:
    reader = Reader(data)
    packet = empty()
    for key in ("enum_values", "chained_subcommand_values", "postfixes"):
        packet[key] = [reader.string() for _ in range(reader.varint())]
    for _ in range(reader.varint()):
        name = reader.string()
        packet["enums"].append({"name": name, "values": [reader.u32() for _ in range(reader.varint())]})
    for _ in range(reader.varint()):
        name = reader.string()
        values = [
            {"sub_command_first_value": reader.varint(), "sub_command_second_value": reader.varint()}
            for _ in range(reader.varint())
        ]
        packet["chained_subcommands"].append({"name": name, "values": values})
    for _ in range(reader.varint()):
        item = {
            "name": reader.string(),
            "description": reader.string(),
            "flags": reader.take("<H"),
            "permission": reader.string(),
            "alias_enum": reader.take("<i"),
        }
        item["chained_subcommand_indexes"] = [reader.u32() for _ in range(reader.varint())]
        item["overloads"] = []
        for _ in range(reader.varint()):
            chaining = bool(reader.byte())
            params = []
            for _ in range(reader.varint()):
                params.append(
                    {
                        "name": reader.string(),
                        "parse_symbol": reader.u32(),
                        "optional": bool(reader.byte()),
                        "param_options": reader.byte(),
                    }
                )
            item["overloads"].append({"is_chaining": chaining, "params": params})
        packet["commands"].append(item)
    for _ in range(reader.varint()):
        name = reader.string()
        packet["soft_enums"].append({"name": name, "values": [reader.string() for _ in range(reader.varint())]})
    for _ in range(reader.varint()):
        symbol, enum = reader.u32(), reader.u32()
        size = reader.varint()
        flags = list(reader.data[reader.at : reader.at + size])
        packet["constraints"].append({"enum_value_symbol": symbol, "enum_symbol": enum, "constraints": flags})
        reader.at += size
    assert reader.at == len(data), "bytes left over"
    return packet


def find(packet: dict, name: str) -> dict | None:
    return next((item for item in packet["commands"] if item["name"] == name), None)


def enum_values(packet: dict, name: str) -> list[str] | None:
    enum = next((item for item in packet["enums"] if item["name"] == name), None)
    return None if enum is None else [packet["enum_values"][index] for index in enum["values"]]


def soft_enum(packet: dict, name: str) -> list[str] | None:
    soft = next((item for item in packet["soft_enums"] if item["name"] == name), None)
    return None if soft is None else soft["values"]
