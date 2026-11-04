---
paths:
  - "src/endgate/command/**"
  - "src/endgate/connection/**"
  - "tests/unit/command/**"
---

# Commands on Bedrock

Velocity injects its commands into the backend's Brigadier graph (`CommandGraphInjector`). A Bedrock client gets no graph: `AvailableCommandsPacket` is BDS's `CommandRegistry` serialised by `serializeAvailableCommands`, so the injector translates each proxy command into the registry's terms, and nothing about the packet can step outside them. Take every symbol from bedrock-headers' `CommandRegistry.h` (`HardNonTerminal`, `Symbol` and its bits) and the PDB, and check it against a capture from a live BDS: join with `endstone-client` and read the `AvailableCommandsPacket` it received. protocol-docs holds only the packet's schema. The conventions below are checked on a vanilla 1.26.52 capture (82 commands, 253 enums, 14 soft enums). BDS's grammar and the invariants injected data must keep are in `registry.md`; the packets' wire facts are in `packet.md`.

- `AvailableCommandsPacket` is the same in every supported version.
- With `announce-proxy-commands` on, `BackendPlaySessionHandler` decodes the backend's `AvailableCommandsPacket`. For each proxy command the player passes `requires` for, it drops the backend's `CommandData` of that name and adds the proxy's, then forwards the packet. The next backend's packet after a switch is injected the same way; the client rolls its registry back before loading each packet, so the old backend's commands do not linger (`registry.md`, "The client's rebuild").
- A proxy command is one `CommandData`: its literal is the name, its `description` is the proxy's own text, and each node that `executes` is one overload holding the parameters along its path, in order.
- Its permission is `Any` and its flags are `128` (`NotCheat`), as `/list`, `/me` and `/tell` carry them; it adds no `CommandName` constraint. endgate decides with `requires`.
- A command whose last parameter is free text also carries `Message` (64), as `/tell`, `/me` and `/say` do (`192`).
- An alias enum is named `<name>CommandAliases`, lists the command's own name and its aliases, and is the `alias_enum` index (`-1` for none). `/tell` has `tellCommandAliases` with `tell`, `w`, `msg`.
- The command's name and each alias are also values of the shared `CommandName` enum, as for vanilla commands.
- A literal on the path is a parameter of an enum with that one value, as `/time` registers `set`, `add` and `query` and `/scoreboard` registers `objectives` and its actions. It is never a chained subcommand and never shares an enum with its siblings.
- A keyword enum is named `<Command><Keyword>` in CamelCase (`TimeModeSet`, `TitleClear`, `ScoreboardAddAction`), its parameter is named for the keyword, and its parameter options are `0`. The injector names `GlistAll`, `EndgateInfo`, `EndgateReload`, `EndgateDump`.
- Several overloads that reach the same keyword use one enum, as `TimeModeSet` serves two `/time` overloads.
- An optional keyword is an optional enum parameter, as `/reload [all]` (`reload_all`).
- Siblings that share their whole tail may share one multi-value enum, as `/permission` does with `PermissionsAction` (`set`, `list`, `reload`). The injector registers one enum per keyword instead.
- Every enum value is lowercase.
- An argument is a parameter whose `parse_symbol` is the `HardNonTerminal` its `ArgumentType` names. Symbol numbers move between versions, so the table is kept per protocol.
- `IntegerArgumentType` is `Int`, `FloatArgumentType` and `DoubleArgumentType` are `Val`, and `LongArgumentType` has no counterpart.
- `BoolArgumentType` is the shared `Boolean` enum (`true`, `false`), as `/teleport`, `/daylock` and `/gamerule` use it. endgate never defines its own.
- `StringArgumentType.word()` and `.string()` are `Id`: one word or one quoted string, as `/playsound`'s `sound` and `/summon`'s `nameTag`.
- `StringArgumentType.greedyString()` is `MessageRoot`, as `/say`, `/me`, `/tell` and `/kick` take it. It is the last parameter and never optional: no vanilla `MessageRoot` is.
- An optional trailing argument is an optional parameter in the same overload, as `/kill [target]` and `/spawnpoint [player] [spawnPos]`. A required parameter never follows an optional one, although `/summon` registers that.
- A command with no arguments at all is one overload with no parameters, as `/list`, `/stop` and `/save`. When the optional argument is a greedy string, the command has two overloads, one with it and one without.
- A player argument is `Selection` where the argument names a player on the backend (`/tell`, `/kick`, `/op`); the client completes it with `@` selectors and the player list it holds. A name that may not be online is `Id` (`/allowlist add <name>`) and completes nothing.
- An argument that `suggests` becomes a soft enum, filled from its `SuggestionProvider` and kept current with `UpdateSoftEnumPacket` (`Add`, `Remove`, `Replace`). This is the only tab completion a Bedrock client has: it completes from the enums it was sent and never asks the server.
- BDS names a soft enum for its content in CamelCase, with no command prefix: plural nouns (`ScoreboardObjectives`, `EntityEvents`, `CameraPresets`), `Values` for a bare list (`TagValues`, `UnlockableRecipeValues`), singular where one thing is named (`FunctionName`, `Tool`, `ItemTag`). A few are snake_case (`poi_type`). Commands listing the same content share one soft enum: `ScoreboardObjectives` serves `/scoreboard` and `/execute`. It can be empty at registration, as `TagValues` and `ScoreboardObjectives` are, and `UpdateSoftEnumPacket` fills it. Checked on the 14 soft enums of a 1.26.52 `AvailableCommandsPacket`; the headers only declare `addSoftEnum`.
- The injector names a soft enum after its argument: `ServerNames` for an argument `server`, `PlayerNames` for `player`, and `WarpNames` for `warp`; an argument already ending in `name` only gets the `s`. Arguments of one name share one soft enum across commands (`/server`, `/glist` and `/send` share `ServerNames`), which holds the values every sharer suggests. None of the names collides with a vanilla enum or soft enum in the 1.26.52 capture, and a plugin picks distinct argument names for distinct content.
- The injector collapses an executable argument whose parent executes into an optional trailing parameter, and drops an overload another one covers with optional parameters: `/glist [server]`, not `/glist` and `/glist <server>`. A keyword and a greedy message are never collapsed.
- The injector sends a command and its aliases as one `CommandData`, grouped by `CommandMeta`. Where the backend already has an enum of an injected name with other values, the injected enum takes the next free name (`shutdownCommandAliases2`): a client merges same-named enums.
- A literal and a soft-enum argument at one position are one soft-enum parameter whose values include the literal. BDS has no overload pair that differs only by a keyword enum and an `Id`-class argument at the same position, and the client could not parse one: it looks a word up by token type before its keyword, so the `Id` overload takes every word (`registry.md`, "Parsing"). BDS folds a keyword into a soft enum that belongs to one command (`Tool` holds `mainhand` and `offhand`), so the injector does too; a shared enum drops it again, and `/glist all` is `all` typed as a word. The `all` of `/glist` is not in `ServerNames`, so `/server` never completes it, and the parser takes it since a soft enum derives `Id` whatever its values are.
- The injector never creates a chained subcommand, a postfix or a factorization. Chained subcommands are `/execute` only (`ExecuteChainedOption_0`, `chained_subcommand_indexes` `[0]`, option `4` on the chained parameter, every overload `is_chaining`); the only postfix parameters are `/xp`'s `l` and `/schedule`'s `t`, `s`, `d`; factorization is not on the wire. The backend's stay untouched.
- `ClientPlaySessionHandler` takes a `CommandRequestPacket` whose first word is a proxy command the player passes `requires` for, runs it, and doesn't forward it. Anything else goes to the backend unchanged.
- `command-rate-limit`, `forward-commands-if-rate-limited` and `kick-after-rate-limited-commands` apply to `CommandRequestPacket`. `tab-complete-rate-limit` has nothing to limit.

## Open

- Whether flags `0` (`Cheat`) or a permission above `Any` make the client refuse a command for a normal player. The client's `canCommandBeUsed` refuses a Cheat command (flag bit 128 clear) while cheats are off, or a command above the player's permission, but its only caller is the autocomplete list, so the parser still accepts it (`registry.md`, "What serialization writes"). `requiresCheatsEnabled` is not decoded.
- Whether a visibility bit keeps a command out of the packet. Vanilla has 15 `CommandName` values with no `CommandData` (`help`, `?`, `agent`, `listd`, `querytarget`), which fits it.
- What `Message` (64) does on the client; `/kick` and `/scriptevent` take a `MessageRoot` without it.
- Which constraint id means what in `constraints`: `[0,1]` on cheat commands, `[1]` on elevated non-cheat ones, `[0,2]` on `OWNER` ones suggest cheats, elevated and host.
