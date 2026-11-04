---
paths:
  - "src/endgate/command/**"
  - "src/endgate/console.*"
  - "tests/unit/command/**"
---

# Velocity's command layer

`proxy.command` is `src/endgate/command/`, and `proxy.command.builtin` is `command/builtin/`.

- `CommandManager` is Velocity's `CommandManager` and `VelocityCommandManager`, with their method names. `BrigadierCommand` wraps a `LiteralCommandNode<CommandSource>`, and `CommandMeta` carries its aliases. `SimpleCommand`, `RawCommand`, `InvocableCommand` and the registrars arrive with a plugin API, which is out of scope.
- There is no event manager, so no `CommandExecuteEvent`. `executeImmediatelyAsync` is the entry point, and it runs the command before returning whether one ran.
- `CommandSource` has `sendMessage` and `getPermissionValue`, which returns a `Tristate`. `Console` and `Player` implement it.
- With no permissions plugin, permissions are Velocity's defaults: the console has every one (`ALWAYS_TRUE`) and a player has none set (`ALWAYS_UNDEFINED`). The built-ins check them exactly as Velocity's do, so a player gets `/server` and the console gets everything. A permission name swaps `velocity.` for `endgate.`.
- The built-ins are `ServerCommand`, `GlistCommand`, `SendCommand`, `ShutdownCommand`, and `EndgateCommand` for `VelocityCommand` (`/endgate info`, `reload` and `dump`; `plugins` and `heap` have no counterpart). They reply in Velocity's English wording from `proxy/src/main/resources/com/velocitypowered/proxy/l10n/messages.properties`.
- A player's command runs on the player's event loop and the console's on the console thread. Anything a command does to another player goes through that player's loop (`threading.md`).
- The console passes each line to `executeImmediatelyAsync`, and replxx's completion callback asks `offerSuggestions`, as `VelocityConsole`'s JLine completer does.
