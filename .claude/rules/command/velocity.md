---
paths:
  - "src/endgate/command/**"
  - "src/endgate/console.*"
  - "tests/unit/command/**"
---

# Velocity's command layer

`proxy.command` is `src/endgate/command/`, and `proxy.command.builtin` is `command/builtin/`, where each built-in has a header and source of its own named after the command: `glist.{h,cpp}` holds `GlistCommand`, `server.{h,cpp}` holds `ServerCommand`, and so on.

- `CommandManager` is Velocity's `CommandManager` and `VelocityCommandManager`, with their method names. `BrigadierCommand` wraps a `LiteralCommandNode<CommandSource>`, and `CommandMeta` carries its aliases.
- `CommandMeta` also carries a description, set with `Builder::description`, since Velocity has none and a Bedrock client shows one for every command. It is plain English in Endstone's style, one sentence in the third person (`Lists the players connected to the proxy.`); vanilla sends translation keys the client resolves, which a proxy command has none of. The injector sends it as `CommandData::description`, empty where the meta has none. `SimpleCommand`, `RawCommand`, `InvocableCommand` and the registrars arrive with a plugin API, which is out of scope.
- There is no event manager, so no `CommandExecuteEvent`. `executeImmediatelyAsync` is the entry point, and it runs the command before returning whether one ran.
- `CommandSource` has `sendMessage`, `sendErrorMessage` (Endstone's `CommandSender`, since Velocity has no error channel) and `getPermissionValue`, which returns a `Tristate`. `Console` and `Player` implement it.
- With no permissions plugin, permissions are Velocity's defaults: the console has every one (`ALWAYS_TRUE`) and a player has none set (`ALWAYS_UNDEFINED`). The built-ins check them exactly as Velocity's do, so a player gets `/server` and the console gets everything. A permission name swaps `velocity.` for `endgate.`.
- The built-ins are `ServerCommand`, `GlistCommand`, `SendCommand`, `ShutdownCommand`, and `EndgateCommand` for `VelocityCommand` (`/endgate info` and `reload`; `plugins` and `heap` have no counterpart, and `dump` is left out because Velocity uploads its report to a paste service). They reply in Velocity's English wording from `proxy/src/main/resources/com/velocitypowered/proxy/l10n/messages.properties`.
- A player's command runs on the player's event loop and the console's on the console thread. Anything a command does to another player goes through that player's loop (`threading.md`).
- The console passes each line to `executeImmediatelyAsync`, and replxx's completion callback asks `offerSuggestions`, as `VelocityConsole`'s JLine completer does.
- `Tristate` and `PermissionSubject` are in `permission/` (`tristate.h`, `subject.h`) after `com.velocitypowered.api.permission`, and `CommandSource` is `command/source.h`. `Tristate` is an enum class with `True`, `False` and `Undefined`.
- `register` is a C++ keyword: `CommandManager::registerCommand`, and a built-in with a class of its own registers with `registerCommand()`. `metaBuilder` is static.
- Replies keep Velocity's words but take their colours from BDS and Endstone, not Velocity's palette. Most replies are plain, as BDS's own `/list` is: `/glist`, `/server`, and `/endgate info`'s copyright and links.
- An error goes through `sendErrorMessage`: unknown commands, Brigadier syntax errors, usage lines, "does not exist", "Only players can run this command", a failed reload and a command that threw. A player gets it with a `§c` prefix, as Endstone's `EndstonePlayer::sendErrorMessage` does. The console logs it with `logger.error`, as `EndstoneConsoleCommandSender` does, so its red comes from the ERROR level.
- Red is for errors only. On the console it comes only from the ERROR level, never from `§c` inside an INFO line.
- Colour beyond that follows Endstone's own commands: a successful reload is `§a`, as Endstone's `/reload` is, and `/endgate info`'s name and version line is `§6`, as Endstone's `/version` header is.
- Velocity's hover and click events and its underline have no Bedrock form and are dropped. The console turns `§` codes into ANSI and the file log strips them (`logging.md`).
- A command source is a `std::shared_ptr<CommandSource>`. `Player::getCurrentServer` is an atomic read, so `/glist` and `/send current` can count players from the console thread.
- `SuggestionsProvider` is private to `CommandManager`. Only `BrigadierCommand` is registrable, so it has Velocity's alias suggestions and the regular Brigadier path, and none of the `SimpleCommand` arguments node or hint parsing. A node's requirement does not filter argument suggestions, as in Velocity.
- `/endgate info` is `VelocityCommand.Info`: the name and version line, the copyright line with start year 2026, vendor EndstoneMC and the GNU Affero General Public License v3, and, for the Endgate build, the website and source links as plain text since there is no click event.
