---
paths:
  - "src/endgate/command/**"
  - "src/endgate/connection/**"
  - "tests/unit/command/**"
---

# BDS's command grammar

The client rebuilds BDS's own `CommandRegistry` from `AvailableCommandsPacket` and parses what the player types with it. Injected commands work only if they fit that grammar. Checked on 1.26.51 (BDS and client databases, decompiled) and the headers for 1.26.40 to 1.26.60; the packet's wire fields are in `packet.md`.

## Symbols

- A `Symbol` is an int: kind bits plus an index, or for hard non-terminals a fixed number. `toIndex()` masks with `0xE00FFFFF`, which drops every kind bit.
- Kind bits: non-terminal `0x100000`, enum `0x200000`, optional `0x400000`, factorization `0x800000`, postfix `0x1000000`, enum value `0x2000000`, soft enum `0x4000000`, chained subcommand `0x8000000`, chained subcommand value `0x10000000`.
- An enum symbol is its index with the enum and non-terminal bits. A soft enum symbol is its index with the soft enum and non-terminal bits (`0x4100000`). An enum value symbol is its index with the enum value bit alone.
- A terminal is a lexer token type, `1` to `33`; `33` is end of input. Everything else is a non-terminal.
- A `HardNonTerminal` is `0x100000` plus a fixed number. There are 95 on 1.26.51 and 1.26.60, and 93 on 1.26.40. Later ones shift when BDS adds one, so the table is per protocol.
- `CommandRegistry::isValid` range-checks a symbol per kind against the registry's own vector: postfixes, enums, chained subcommands, chained subcommand values, optionals, factorizations, enum values, soft enums, then the hard non-terminal count and the 33 terminals.
- The packet carries no optional or factorization symbols, only an optional flag per parameter. The registry builds both kinds from the overloads.
- Chained subcommand and chained subcommand value symbols: see "Chained subcommands".
- Factorization symbols: see "Factorization".
- Postfix symbols: see "Postfixes".

## How a type becomes a symbol

- A parameter's type is a `ParamParseRule`: a parse function and the symbol the grammar uses for it. Static rules are filled by dynamic initialisers.
- 1.26.51: `int` is `Int`, `float` is `Val`, `std::string` is `Id`, `CommandMessage` is `MessageRoot`, `CommandSelector<Player>` is `Selection`, `CommandRawText` is `RawText`.
- `Id` is one word or one quoted string. Its parse function strips the surrounding quotes, and a backslash escapes the next character.
- `MessageRoot` is the rest of the line.
- `bool` and every `enum class` parameter take an enum symbol, set when the enum is registered, not a hard non-terminal.

## Enum values and soft enums

- `addSoftTerminal` registers an enum value. A known text returns its existing symbol, so all enums share one value pool keyed by text.
- A text with an uppercase letter is refused: BDS logs "Keywords in commands need to be lower case. '%s' was not registered." and the caller gets symbol 0. The client runs the same code on every enum value it loads.
- A new value adds one rule that derives `Id`, with a process step that records the value's symbol in the parse tree.
- `addSoftEnum` takes a name and values. A new name gets a soft enum symbol and one rule that derives `Id`; a known name takes the add-values path instead.
- That rule does not depend on the values. Adding, removing or replacing a soft enum's values never changes the grammar or the parse tables, so `UpdateSoftEnumPacket` needs no rebuild.
- The values are for completion and for validation after the parse. Whether a word outside them is rejected is not decompiled.
- `removeSoftEnumValues`, `addSoftEnumValues` and `setSoftEnumValues` exist on the registry; the last two send `UpdateSoftEnumPacket` through the registry's network callback.

## Parsing

- `CommandRegistry::Parser` is a table-driven LL(1) parser: a stack of symbols, one token of lookahead, no backtracking.
- The lexer gives each word a token type and an identifier info: the enum value or postfix terminal whose text it equals, or 0.
- A symbol on top of the stack that equals the token type or the identifier info is consumed.
- Otherwise the parser looks up the predict table for the pair (token type, stack top), then for (identifier info, stack top). A hit pops the top and pushes the rule's derivation in reverse, skipping `Epsilon`, and runs the rule's process step on the parse tree.
- A miss ends the parse with an error. If the stack top's parent in the parse tree is `Command`, the error is `commands.generic.unknown` with the word as its parameter. Anything else is `commands.generic.syntax` with the word.
- At end of input, every symbol left on the stack must have a predict entry under token type 33, which is how nullable symbols (optional parameters) finish.
- An enum is a non-terminal whose first set is its enum value terminals, so a hard enum is told apart by identifier info. A soft enum predicts on `Id`, so any word reaches it.

## Overloads become rules

- `registerCommand` makes a signature. `registerOverloadInternal` adds each overload and `setupOverloadRules` rebuilds the signature's rules.
- `setupOverloadRules` discards the rules, factorizations and optionals the signature built earlier, orders its overloads with a fixed comparator, runs `buildRules` over them, and adds the rule that ties the command symbol to the `Command` non-terminal.
- Left factoring (factorization symbols) and optional chains (optional symbols) let overloads that share a prefix or end in optional parameters coexist in an LL(1) table. How `buildRules` factors them is not decompiled.
- A parse table is built per version from first sets, follow sets and a nullable set, then a predict table. `loadRemoteCommands` erases the cached tables.
- The predict table is a map from (token, non-terminal) to a rule index. `CommandRegistry::Parser`'s constructor fills it from a flat list of (token, non-terminal, rule) triples. It looks each pair up and overwrites the stored rule when the pair exists, else appends. The last triple wins; nothing is logged, asserted or dropped. Checked on 1.26.51, BDS (`0x14121f2a0`, store at `0x141221010`) and client (`0x144a531e0`, store at `0x144a54f80`): identical, so a clash resolves the same way in execution and in completion.
- The only log in that constructor is "Missing rule derivation, rule: %s, derivation: %s", for a rule with no derivation. A clash prints nothing.
- The loser is not removed. Its rule stays in the grammar, but no word predicts it for that pair, so it parses as a syntax error. The surviving rule is the one emitted last. That follows the rule order within a non-terminal; the order was not traced.
- Keyword terminals (enum value kind bit) take an extra pre-insert check that can skip the insert (client `0x144a54fce`..`0x144a55266`): it looks the keyword up in a map built earlier in the constructor, and skips when the entry's second field is not -1 and its flag byte is set. What that map holds was not traced.
- Lookup order in `Parser::_parse`, checked on 1.26.51 client (`0x144a86d28`..`0x144a86f87`): the key (token type, stack top) is looked up first. The identifier-info key is tried only if that misses and the identifier info is nonzero. So a word that has a (token type, non-terminal) entry never reaches a keyword entry at the same non-terminal. See "Checklist" for what that means for a keyword next to an `Id`.

## The client's rebuild

- `loadRemoteCommands` itself clears nothing: it starts straight on the packet's enum values. A second packet through it alone would add to the old registry.
- `ClientNetworkHandler::handle_5` (`0x141306410`) first makes two virtual calls: slot 79 of the object at `this+0x58` returns an object, and slot 197 of that object is called with `this+0x58`. By its call shape this is `MinecraftGame::setupCommandParser` (`0x14080b620`), which is what the handler reaches; the vtable slot was not matched directly. That function calls `popState` if the registry's state stack is non-empty, then `pushState`, then re-registers the client's own enums (`initCoreEnums`) and commands (`ClientCommands::setupStandard`). So every AvailableCommandsPacket first rolls the registry back to the snapshot taken before the previous remote load, then loads the new packet: a server switch replaces the old backend's commands, it does not merge. Checked on 1.26.51; the rollback itself (`popState`, `0x144a946c0`) was not read.
- An enum in the packet whose name the registry already has (the client's own enums, or a repeat in the same packet) is merged: the packet's values are added to it through `addEnumValuesToExisting` (lookup in the enum name map, `0x144a9a2b1`). A new name goes through `_addEnumValuesInternal`.
- An enum value index in an enum that is past the end of the packet's value list becomes the first enum value instead of failing (`0x144a9a328`, `0x144a9a330`). Nothing is logged.
- A command name with an uppercase letter is not registered: `registerCommand` calls `addSoftTerminal` on the name first, and returns when it gets symbol 0 (`0x144a7897b`). `loadRemoteCommands` then calls `findCommand` on the same name and writes a byte through the result at once (`0x144a9af1d`) with no null check, so the client dereferences null. Read from the instructions, not run.
- A command's alias enum index (the signature's alias field) is range-checked against the packet's enum list; past the end, the alias step is skipped and the command stays (`0x144a9af32`..`0x144a9af50`, jump to `0x144a9b430`). A negative index is skipped the same way.
- `registerCommand` has no duplicate-name path in the part read; a repeated command name goes to the same code as a new one. Whether the signature map keeps the first or the second was not read.
- `loadRemoteCommands` replays registration, in call order: enum values (`addSoftTerminal`), postfixes (`addPostfix`), chained subcommand values, enums (`_addEnumValuesInternal`, and `addEnumValuesToExisting` among its callees), chained subcommands, soft enums (`addSoftEnum`), commands (`registerCommand`, with `findCommand`), overloads (`setupOverloadRules`, `setupChainedSubcommandOverloadRules`), then constraints.
- The only message it can log is the lowercase-keyword one. It has no assert or disconnect string. It range-checks enum value indices and the alias enum index as above. The other indices (enum value text indices in `enum_values`, soft enum and command parameter symbols, constraints) were not checked, so treat those as fatal when out of range.

## When BDS sends it

- `serializeAvailableCommands` has two callers on 1.26.51: `ServerNetworkHandler::sendLoginMessageLocal` and the sub-client variant. BDS sends the packet once per login.
- No other code path in BDS calls it, so a permission change, an op or a reload does not resend it.

## What serialization writes

- `serializeAvailableCommands` copies the registry's tables as they stand: `enum_values`, chained subcommand values and postfixes are the registry's own vectors, passed unchanged; each registry enum becomes one `EnumData` in registry order, its values copied from the first field of each stored pair; soft enums and chained subcommands likewise. Nothing is filtered to the enums a command uses.
- A parameter's symbol is the first of: the enum-or-postfix symbol if not -1, then the chained subcommand symbol if not -1, then the parse rule's symbol, else 0. A Basic parameter carries no enum or chained symbol, so its symbol is its `ParamParseRule` symbol. The packet has one symbol per parameter; the optional flag and the options byte come from the parameter's `mIsOptional` and `mOptions`.
- Filtering of commands by flag or visibility during serialization was not decompiled.
- Flag values (headers): visibility bits are 2 (hidden from command blocks), 4 (hidden from players), 8 (hidden from automation); 16 local, 32 disallowed, 64 Message, 128 NotCheat, 256 Async, 512 NotEditor, 1024 teacher-always-allowed.
- `CommandRegistry::canCommandBeUsed` (client `0x144a91d40`, checked on 1.26.51) returns false when the player's permission level is below the command's, or when cheats are off and the command is unknown or does not have NotCheat (128), or when its flag 512 is set and the caller passed the editor flag. Its only client caller is the autocomplete update (`IntellisenseHandler::_updateAutoCompleteMessages`), so it shapes the suggestion list, not what the parser accepts. BDS has no name for it in the 1.26.51 database; the BDS side was not found.
- `registerCommand` ORs its two flag arguments. A feature-toggle callback on the registry can clear the visibility bits (mask `0xFFF1`) for a command; a command whose three visibility bits are all set skips the alias-enum registration (`0x144a78b9b`).

## What BDS does with a CommandRequest

- The dispatcher is a virtual call to `ServerNetworkHandler::handle_1` (`0x1409763e0`). It builds a `CommandContext` from the packet's origin data and command string, then calls `MinecraftCommands::executeCommand`.
- An origin that cannot be built (`CommandOrigin::fromCommandOriginData` returns null) disconnects the client with `InvalidCommandRequestPacket`.
- The command string reaches `Parser::parseCommand` unchanged; neither `createCommandContext` nor `executeCommand` strips a leading `/`. Whether the parser's lexer accepts one was not checked.
- `executeCommand` builds a `CommandOutput` and sends it with `MinecraftCommands::handleOutput` only when it has messages or is a data-set output. The packet written by `handleOutput` was not decompiled.
- A parse failure adds the parser's message id (`commands.generic.unknown` or `commands.generic.syntax`, with the word as parameter), sets `statusCode` to `FailedToParseCommand` and sends the output. Unknown commands take this path, so the player gets the usual error line.
- With commands disabled in the level (and the command not exempt) the output is `commands.generic.disabled`; an invalid origin gives `commands.generic.invalidOrigin`.
- A command that runs and prints nothing sends nothing. The output type, picked from the origin and gamerules, can be zero, and then messages are not even added.

## Symbol encodings

Checked on 1.26.60 (Windows server PDB, disassembly) and a 1.26.52 capture of the whole registry.

- `toIndex()` masks with `0xE00FFFFF`; `serializeAvailableCommands` uses it for the alias enum.
- An enum is its index | `0x300000` (enum + non-terminal). A soft enum is its index | `0x4100000`. A postfix is its index | `0x1000000`, with no non-terminal bit. A chained subcommand is its index | `0x8100000`.
- A factorization is its index | `0x900000`, and an optional is its index | `0x500000`. Neither reaches the wire.
- `ParamData.parse_symbol` is the raw symbol: the enum or postfix symbol if set, else the chained subcommand symbol, else the type's rule symbol. The capture has `0x1000003` for a postfix parameter and `0x8100000` for /execute's chained parameter.
- A chained parameter's index is the position in `chained_subcommands`: any value below its length. Vanilla has one entry, so every chained parameter is `0x8100000`.
- No code in the chained-subcommand paths of 1.26.60 builds a symbol with the chained-value bit `0x10000000`. A chained value is a plain index into `chained_subcommand_values`. Whether the client builds a value symbol is open.

## Chained subcommands

Checked on 1.26.60 and the 1.26.52 capture.

- Only `/execute` uses them. The capture has one `chained_subcommands` entry, `ExecuteChainedOption_0`, and `chained_subcommand_indexes = [0]` on `/execute` alone.
- `chained_subcommand_values` holds the keywords `as at in positioned rotated facing align anchored if unless run`. They are lowercase, with the same "Keywords in commands need to be lower case" refusal as enum values.
- Each entry of a chained subcommand is a pair `(value index, enum index)`. The first is the keyword's index in `chained_subcommand_values`. The second is the index in `enums` of the enum that is the keyword parameter of that subcommand's overloads.
  - The capture: `as` -> 77 `Option_As`, `at` -> 78, `in` -> 79, `positioned` -> 80, `rotated` -> 81, `facing` -> 82, `align` -> 84, `anchored` -> 85, `if` and `unless` -> 86 `Option_If_Unless`, `run` -> 91.
  - `ExecuteChainedSubcommand::setup` looks the enum up by name when it builds the pairs.
- A command lists its chained subcommands in `chained_subcommand_indexes`. The serializer copies the signature's list.
- A chaining overload is an ordinary overload with `is_chaining` set. `registerSubcommandChainingOverload` registers the overload, then sets the flag on the one it added. `finalizeChainedSubcommandOverloadRules` then calls `setupChainedSubcommandOverloadRules`, which builds rules from the chaining overloads (process step `expand`).
- All 18 of `/execute`'s overloads in the capture are chaining. The last parameter is the chained parameter (`0x8100000`, `param_options` 4, the "as chained command" option). It is required in most overloads and optional in the `if`/`unless` ones, so the chain can end there.
- The parse function for the chained parameter is the chained subcommand's own (`parseEnum<CommandChainedSubcommand>`). It parses the next subcommand into a nested `Command`, which a `CommandChainedSubcommand` holds.
- The wire allows 16 chained subcommands with 32 values each.
- How the client moves from one keyword to the next, and the exact rule shapes `setupChainedSubcommandOverloadRules` adds, are not decompiled.

Injector:
- Never touch `chained_subcommand_values`, `chained_subcommands` or any command's `chained_subcommand_indexes`.
- Proxy commands have no chained parameter and no chaining overload. Set `is_chaining` false.
- Keep every enum index stable. A chained pair's second value and every enum parameter symbol point into `enums`, so append new enums at the end and never reorder or remove one.
- Dropping the backend's `/execute` leaves its chained entries orphaned but valid. Do not drop it for a proxy command.

## Factorization

Checked on 1.26.60 (`buildRules`, disassembled) and the 1.26.52 capture. Not decompiled; the grouping rules below come from reading the instructions.

- Factorization symbols exist only in the built grammar. The packet carries no factorization, only each parameter's optional flag. The client derives them from the overloads.
- `buildRules(signature, overloads, firstParam)` builds the rules for one position and recurses to the next. Its first step allocates a new factorization non-terminal for the position, which is the left side of every rule it adds there. The new symbol takes the next index in the registry's factorization list. The signature remembers the first.
- It returns plain `Epsilon` with no new symbol when every overload has ended at this position.
- It adds `F -> Epsilon` when any overload ends here or has an optional parameter here. That is how a shorter overload coexists with a longer one that shares its prefix.
- Overloads are grouped by their parameter symbol and version range. Identical symbols are factored; anything else gets its own rule. Groups are taken in the order their first overload appears.
- The symbol of a parameter is the same value the wire carries: enum or postfix symbol, else chained symbol, else the type's rule symbol. Two parameters factor only if they are the same enum index, the same soft enum, or the same type. A literal enum with the same words but another index is another symbol.
- For a group, the run is the first parameter plus the optional parameters that follow it identically in every overload of the group. The run is cut at the first parameter that differs, is required, or where an overload ends.
- When some overload continues past the run, the rest is `buildRules` again from the next position, and the rule is `F -> run F'`. When every overload ends in the run, the rule is `F -> run` alone.
- Optional parameters are not expanded into extra overloads. A run of optionals becomes a chain of optional non-terminals: `Opt -> param Opt'`, `Opt -> param`, and `Opt -> Epsilon`. Chains are reused when their parameters match.
- Each overload keeps the symbols it was built from, for later rule lookup.
- The rules are added without a conflict check. If two groups at one position predict the same word, the predict table decides, and it overwrites silently (see Open).
- Vanilla keeps every shared prefix on one symbol. In the capture, 315 same-position enum pairs share no value text, and `/execute` reuses `Option_Positioned` and `Option_If_Unless` across its overloads.

Combinations at one position (after the same prefix) that conflict, so the injector must refuse them:
- Two different enum symbols whose values share any text, including the same word in two enums made for two overloads. The factor never forms and both predict the word.
- A literal enum next to a parameter that derives `Id`: a string, a selector, a soft enum, a file path or raw text. The `Id` token type is looked up first, so the literal's overload is never reached for any word.
- Two different `Id`-deriving parameters: string and selector, string and soft enum, selector and soft enum, two different soft enums. Each predicts on `Id`.
- An optional parameter followed by a required one whose first token overlaps it. The optional is taken greedily and the required one then finds nothing. Optional enum then required message is the common case (Endstone #559).
- A message or raw-text parameter that is not last.

Combinations that factor cleanly (all in the 1.26.52 capture):
- The same symbol in several overloads, then different continuations.
- Two enums with disjoint values.
- A shorter overload that is a prefix of a longer one.
- Int next to a literal enum or a postfix; Position or PositionFloat next to a selector or a literal enum; Id next to Position or PositionFloat; RVal, a comparison operator or a block state array next to a literal enum; a block state array next to a chained parameter.
- An optional parameter followed by a required enum (`/summon`).

Injector:
- Proxy commands (`/server`, `/glist`, `/send`, `/shutdown`, `/endgate info|reload|dump`) never need factorization. Do not put factorization data on the wire; the client derives it.
- One enum entry per distinct keyword of a command, shared by every overload that uses it. Never emit a second enum with the same words, or the same name.
- A literal and a name in the same position cannot both exist. Make `/glist all` or `/server <name>` one parameter: put `all` among a soft enum's values.
- Keep the shapes to the clean list above. For `/endgate`, one enum per subcommand word with disjoint words.
- Proxy commands do not interact with the backend's grammar unless they replace a command of the same name: each command has its own command symbol and its own factorization chain.

## Postfixes

Checked on 1.26.60 and the 1.26.52 capture.

- A postfix is a text a number can end with, such as the `l` in `/xp 5l`. It is a terminal, `index | 0x1000000`.
- Registration is lazy. A parameter of type postfix names its postfix text. `registerOverloadInternal` lowercases it, looks it up in the registry's postfix list, appends it if absent, and sets the parameter's symbol to the postfix symbol. For a new postfix it also adds the rule `Id -> postfix`.
- Vanilla uses postfixes in two commands: `/xp` (`l`) and `/schedule` (`t`, `s`, `d`). The capture's table is `[t, s, d, l]`.
- The postfix parameter is a separate overload of the same command. In the capture, `/xp` has `<amount:Int> [player:Selection]` and `<amount:postfix:l> [player:Selection]`, and `/schedule add` has the `Int` form plus one overload per postfix.
- The wire holds the table as a list of strings (`postfixes`) and the parameter symbol `0x1000000 | index`. There is no other postfix field.
- `findPostfix` takes the token text and returns the postfix terminal. How the lexer splits `5l` into a number and its suffix is not in the server database; open.
- Int next to a postfix and one postfix next to another coexist cleanly at one position in the capture.

Injector:
- Proxy commands need no postfix. Do not add to `postfixes`; leave the table and every index into it as the backend sent it.
- Avoid a literal or a soft-enum value equal to a backend postfix text (`l`, `t`, `s`, `d`). The `Id -> postfix` rule makes that word an identifier with the postfix as its identifier info. Whether this changes how the word parses is open.

## Open

- Which rule is emitted last for a clashing pair (the comparator in `setupOverloadRules` and the order `buildRules` adds rules), and what the map in the keyword pre-insert check holds. Not traced in the third pass.
- Whether every `Id`-deriving type (string, selector, soft enum, file path, raw text) predicts on the one `Id` entry. Not decoded.
- The packet written by `MinecraftCommands::handleOutput`, and what the client shows when a request gets no output.
- Whether BDS's `serializeAvailableCommands` filters by flag, permission or cheats; `requiresCheatsEnabled`; what flag 64 (Message) changes; constraint ids; `registerAlias` order; soft enum values with spaces.
- Which signature `registerCommand` keeps for a repeated command name.
- Whether `setupChainedSubcommandOverloadRules` and the lexer's postfix split (`findPostfix`) behave as hoped; see the Chained subcommands and Postfixes sections.
- How `buildRules` factors shared prefixes.
- Whether the parse rejects a soft enum word outside its values.
- Out-of-range indices in `loadRemoteCommands` other than enum values and the alias enum.
- 1.26.60: the BDS database has no names, so the symbol table there comes from headers only.

## Checklist for injected data

- Every enum value text, command name and alias is lowercase.
- Every index (enum values in enums, enums and soft enums in parameter symbols, alias enums, chained subcommands, constraints) is below the length of its table.
- `enum_values` holds each text once; enums point at it by index, and a text shared by two enums is one entry.
- A keyword next to an `Id`-deriving parameter at one position never matches: the word's token type is looked up before its keyword, and the `Id` entry hits. `/glist all` and `/glist <server>` cannot be two overloads; use one string or soft enum parameter.
- A server switch needs no cleanup: the client rolls back to its own commands before loading each packet, so send the new backend's full packet.
- A command name is unique across `commands`. Replace the backend's entry of a clashing name instead of adding a second.
- A soft enum is in `soft_enums` before any parameter refers to it, and its name is unique.
- Parameters in an overload are in order, required first, optional after. A message parameter is last.
- Overloads of one command differ at a position the parser can tell apart: a literal, or a type with a different first token. Two overloads that differ only by a soft enum, a string or a selector at one position are ambiguous, since each predicts on a word.
- A proxy command's literals and arguments use only symbols the target protocol defines.
