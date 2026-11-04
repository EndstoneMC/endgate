---
paths:
  - "src/endgate/command/**"
  - "src/endgate/connection/**"
  - "tests/unit/command/**"
---

# Command packets

The wire facts for `AvailableCommandsPacket` (76), `CommandRequestPacket` (77), `CommandOutputPacket` (79) and `UpdateSoftEnumPacket` (114). What a symbol means to the client's parser is in `registry.md`; how the injector uses them is in `bedrock.md`. Checked on protocols 2168 to 2225 (1.26.40 to 1.26.60) against bedrock-protocol's goldens, EndstoneMC/protocol-docs (`r26_u4` to `r26_u7`), Mojang's protocol docs (2235 preview), bedrock-headers (`android/r26_u4` to `r26_u6`), gophertunnel and Cloudburst. No capture of a live `AvailableCommandsPacket` was available, so "from a dump" below means a schema dump or a hand-built golden.

- Every packet in this file has the same wire shape on 2168, 2169, 2193 and 2225. Protocol-docs `r26_u4` to `r26_u6` are byte-identical for all of them.
- Integers are little-endian. A count is a `uvarint32`. A string is a `uvarint32` length and the bytes. `uint32` below is fixed four bytes; only the places that say `uvarint32` are variable.

## AvailableCommandsPacket

Eight lists, in this order. An empty packet is eight zero bytes.

1. `enum_values`: strings. The pool every enum value is drawn from.
2. `chained_subcommand_values`: strings. The pool for chained subcommands.
3. `postfixes`: strings.
4. `enums`: each a `name` string and a count of `uint32` indices into `enum_values`.
5. `chained_subcommands`: each a `name` string and a count of value pairs, each pair two `uvarint32`.
6. `commands`.
7. `soft_enums`: each a `name` string and a count of strings. The values are inline, not indices.
8. `constraints`: each a `uint32` index into `enum_values`, a `uint32` index into `enums`, and a count of bytes.

- Enum value indices are a fixed `uint32` on every protocol in range. The old width by `enum_values` size (byte, short, int) ended when the schema went to cereal (gophertunnel 1.21.130, protocol 898). Protocol-docs, Mojang's docs and bedrock-protocol's golden agree.
- The pair values in `chained_subcommands` are `uvarint32`, unlike the enum indices. Mojang's docs mark them compressed.
- A `commands` entry:
  - `name`: string.
  - `description`: string.
  - `flags`: `uint16`.
  - `permission`: a string, the lowercase level name: `any`, `gamedirectors`, `admin`, `host`, `owner`, `internal`. Not a byte.
  - `alias_enum`: `int32`, an index into `enums`, `-1` for none.
  - `chained_subcommand_indexes`: a count of `uint32` indices into `chained_subcommands`.
  - `overloads`: a count; each an `is_chaining` bool and a count of parameters.
- A parameter is `name` string, `parse_symbol` `uint32`, `optional` bool, `param_options` `uint8`, in that order.
- A `constraints` byte is an ordinal: 0 cheats enabled, 1 elevated permissions, 2 host permissions, 3 allow aliases. Gophertunnel and Cloudburst agree; the headers' flag values for the same four are 1, 2, 4 and 8.
- Limits in the schema: an `enum_values`, `postfixes` or `chained_subcommand_values` count has none; `chained_subcommands` at most 16 entries, each at most 32 pairs; a command at most 250 overloads and 250 chained indexes; a constraint at most 250 bytes; `description` at most 1000 characters; `name` at most 512 in protocol-docs and 1000 in Mojang's docs.

### What each index references

- `enums[].values[]`: position in `enum_values`.
- `commands[].alias_enum`: position in `enums`.
- `commands[].chained_subcommand_indexes[]`: position in `chained_subcommands`.
- `constraints[]`: first field a position in `enum_values`, second a position in `enums`. These are plain indices, not symbols.
- A parameter's `parse_symbol` carries an index to `enums`, `soft_enums`, `postfixes` or `chained_subcommands`, tagged by a kind bit (see below).
- The packet carries no optional or factorization symbols; the client builds those from the `optional` flag. Mojang's docs and the protocol-docs fields have no place for one.

### Chained subcommands

- `chained_subcommand_values` is a pool of texts. A `chained_subcommands` entry is a name and pairs `(first, second)`.
- `first` is a position in `chained_subcommand_values`. Mojang calls it "the sub command value id"; gophertunnel and Cloudburst read it the same way.
- `second` is open. Mojang: "the enum id". Gophertunnel: a basic type with no flag bits. Cloudburst: a second position in `chained_subcommand_values`. The headers declare the registry's pair as an index and an `unsigned int`. Only a decoded `/execute` settles it.
- A command lists the `chained_subcommands` it uses in `chained_subcommand_indexes`. A command with none sends an empty list.
- An overload's `is_chaining` says its parameters use chained subcommands (gophertunnel). Every overload of a plain command sends false.
- Parameters of a chained overload are `ParamData` like any other. A chained subcommand parameter has the chained bit (below); a parameter that takes the next command in the chain (`/execute ... run <command>`) has param option `4` (see "param_options").
- No dump with `/execute` was available. Gophertunnel and Cloudburst read a chained parameter's symbol as `0x8100000` when its index is 0, which implies the non-terminal bit is set with the chained bit; this is not confirmed for other indices. Open for IDA.

### Postfixes

- `postfixes` is a list of strings, such as `L` for a level count.
- A parameter that takes a postfix has `parse_symbol` = the postfix's position in `postfixes` | `0x1000000`. Cloudburst's encoder and decoder do not set the non-terminal bit `0x100000` on it, and its decoder accepts the symbol only when the postfix bit is set. The parameter has no basic type of its own; the client knows only the postfix.
- No dump with `/xp` was available, so which parameter of `/xp` uses it is not confirmed here.

## ParamData

`parse_symbol` is a `Symbol`: kind bits plus an index or, for a basic type, a fixed number.

| Kind | `parse_symbol` | Index |
| --- | --- | --- |
| Basic type | `0x100000 + n` for `HardNonTerminal` number `n` | none |
| Enum | `0x300000 \| i` (`0x200000` and `0x100000`) | `i` in `enums` |
| Soft enum | `0x4100000 \| i` (`0x4000000` and `0x100000`) | `i` in `soft_enums` |
| Postfix | `0x1000000 \| i` | `i` in `postfixes` |
| Chained subcommand / next command | `0x8000000` with `0x100000`; index rule open | see above |

- `0x100000` is the non-terminal bit; every kind but a postfix carries it. A reader that sees neither it nor the postfix bit rejects the parameter (Cloudburst throws).
- `bool` and every enum-typed parameter take the enum kind, not a basic type: the `Boolean` enum of `true` and `false` is sent in `enums`.
- `optional` is a flag the client turns into its own optional symbols. Required parameters come before optional ones.
- `param_options` is a bit set:
  - `1`: `EnumAutocompleteExpansion`. The enum's values are not printed in the usage line and appear only as completions (gophertunnel calls it "collapse enum"; Cloudburst "suppress enum autocompletion").
  - `2`: `HasSemanticConstraint`. The enum value carries an entry in `constraints`.
  - `4`: `EnumAsChainedCommand`. The parameter takes the rest of the chain as a command.
  - `3` is not a defined option.

## HardNonTerminal numbers

The type of a basic parameter. The numbers are `0x100000` plus the offset. They are equal on 2168, 2169, 2193 and 2225 for everything endgate uses; bedrock-headers `r26_u4` (1.26.40), `r26_u5` (1.26.51) and `r26_u6` (1.26.60) agree, and so do Cloudburst's tables for protocol 975 and later.

| Type | HardNonTerminal | 2168 / 2169 | 2193 | 2225 |
| --- | --- | --- | --- | --- |
| int | `Int` | `0x100001` | `0x100001` | `0x100001` |
| float | `Val` | `0x100003` | `0x100003` | `0x100003` |
| string, one word or quoted | `Id` | `0x100038` | `0x100038` | `0x100038` |
| greedy message | `MessageRoot` | `0x100044` | `0x100044` | `0x100044` |
| raw text | `RawText` | `0x100046` | `0x100046` | `0x100046` |
| player target | `Selection` | `0x100008` | `0x100008` | `0x100008` |
| command | `Command` | `0x100057` | `0x100059` | `0x100059` |
| slash command | `SlashCommand` | `0x100058` | `0x10005a` | `0x10005a` |

- `float` is `Val`, not `Float` (`0x100002`): `0x100002` is not used on the wire. Cloudburst leaves id 2 unused and sends float as 3; gophertunnel fixed its float constant to 3 in 2026. The headers name 2 `Float` and 3 `Val`; that the registry maps `float` to `Val` is in `registry.md`.
- `Id` is `std::string`'s symbol (`registry.md`). Cloudburst and gophertunnel call 56 `String`.
- `CommandMessage` is `MessageRoot` in `registry.md`. Cloudburst and gophertunnel name 67 (`Message`) and 68 (`MessageRoot`) separately, and servers built on them send either. Which one the client takes for a trailing greedy argument is open; use `MessageRoot` as BDS does.
- `PositionFloat` (`0x100041`) is a position; `Position` (`0x100040`) is a block position. Cloudburst and gophertunnel name them the other way round (`BlockPosition` 64, `Position` 65).
- bool is not a type: it is an enum (see above).
- What moves between protocols: 1.26.40 and earlier have 93 hard non-terminals; 2193 added `PoiTag` (`0x100057`) and `PoiType` (`0x100058`) after `ClockTimeMarkerName` (`0x100056`), so `Command` and everything after it moved up by two. 92 on 1.26.0 and 1.26.10. Nothing endgate needs below `ClockTimeMarkerName` has moved since 1.26.0.
- `r26_u4` has no separate header set for 2169 (1.26.45); 2169 is taken as equal to 2168. Open.
- 1.26.60 (2225) comes from the `r26_u6` headers only; the BDS database for it has no names. Open until a decoded dump confirms.

## UpdateSoftEnumPacket

- Fields in wire order: `enum_name` string, `values` list of strings, `type` `uint8`.
- `type`: `0` Add appends the values, `1` Remove drops the listed values, `2` Replace (gophertunnel `Set`) makes the values the whole list.
- The name must be one sent in `soft_enums`. The client keys on the name only.
- The packet changes the completions; it never changes the grammar (`registry.md`).
- Golden: `kits` with values `alpha`, `beta` and type Remove is `04 'kits' 02 05 'alpha' 04 'beta' 01`.

## CommandRequestPacket

- Fields in wire order: `command` string, `origin`, `internal_source` bool, `version` string.
- `command` is at most 512 characters in protocol-docs and the headers (`sMaxCommandSize`), 1000 in Mojang's docs. The packet as a whole is capped at 550 bytes in the headers.
- The line carries the leading `/`: gophertunnel documents it as the raw entered line and bedrock-protocol's golden is `/say hi`. Not seen in a capture. Open.
- `origin` is `CommandOriginData`:
  - `type`: a string, the lowercase origin name (`player`, `automationplayer`, `devconsole`, `test`, `dedicatedserver`, `commandblock`, `minecartcommandblock`, `clientautomation`, `entity`, `virtual`, `gameargument`, `entityserver`, `precompiled`, `gamedirectorentityserver`, `scripting`, `executecontext`). Protocol-docs allows a client to send only `player`, `automationplayer`, `test` and `devconsole`.
  - `uuid`: 16 bytes, each 8-byte half little-endian. Unique per run of a command.
  - `request_id`: a string of at most 39 characters; non-empty only for a websocket origin.
  - `player_id`: `int64`, always eight bytes. Gophertunnel wrote it only for `devconsole` and `test` before the cereal move and writes it always now.
- `version`: the `CurrentCmdVersion` name as a lowercase string; the newest is written `latest`. What a live client sends is not captured. Mojang's docs show it as an `int32`; the cereal wire is the string (bedrock-protocol golden, gophertunnel `Version string`).
- The server does not echo the request.

## CommandOutputPacket

- Fields in wire order: `origin_data` (`CommandOriginData`), then the output.
- Output: `type` string (`none`, `lastoutput`, `silent`, `alloutput`, `dataset`, lowercase), `success_count` `uint32` fixed, `messages`, `data`.
  - `messages`: a count; each a `message_id` string, a `successful` bool, a count of `params` strings.
  - `data`: an optional string, a presence byte then the string. Absent is `00`.
- Mojang's docs show `type` as an `int32`; the cereal wire is the string (bedrock-protocol golden).
- The reply echoes the request's `origin_data`: type, uuid and request id, which is how the client matches it. Gophertunnel documents "the same CommandOrigin should be sent". Whether BDS echoes `player_id` or writes its own is open.
- Gophertunnel notes that vanilla sends `alloutput`.
- A `raw_message` optional string is added to each message from protocol 2250 (`r26_u7`, 1.26.70 preview); not on 2168 to 2225.

## No completion request

- No packet asks the server for suggestions. `MinecraftPacketIds` on `r26_u6` has none; neither do protocol-docs `r26_u6` or Mojang's 2235 docs. The commands-related packets are `SetCommandsEnabled` (59), `AvailableCommands` (76), `CommandRequest` (77), `CommandBlockUpdate` (78), `CommandOutput` (79), `UpdateSoftEnum` (114) and `SettingsCommandPacket` (140).
- The client completes from the last `AvailableCommandsPacket` plus `UpdateSoftEnumPacket`.

## Changes from 2168 to 2225

- None to the layout of any packet above.
- `CurrentCmdVersion` grows: 50 is the newest on 2168 and 2169, 52 on 2193, 55 on 2225. The newest value is always written `latest`, so a proxy never sees the number.
- 2193 inserts `PoiTag` and `PoiType` into `HardNonTerminal`; `Command` and later symbols move up by two.
- `CommandOutputMessage` gains an optional `raw_message` at 2250, after this range.

## Open

- What `second` in a chained subcommand pair is.
- The exact symbol bits of a chained subcommand parameter beyond index 0.
- Whether the client takes `Message` (`0x100043`) and `MessageRoot` (`0x100044`) identically.
- The leading `/` of `command`, and `player_id` in a reply, on a live client.
- 2169 and 2225 `HardNonTerminal` numbers beyond the headers.
