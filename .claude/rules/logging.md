---
paths:
  - "src/**"
---

# Logging

Log what Velocity logs, when it logs it, at the same level and in the same words. Adapt only where Bedrock differs.

- Name each logger after its class, as Velocity's `LogManager.getLogger(X.class)` does, with no namespace (`LogManager::getLogger("ProxyServer")`, never `"endgate::ProxyServer"`), and hold it as a member.
- The layout is Velocity's `log4j2.xml`:
  - console: `[HH:mm:ss LEVEL]: message`, as Velocity prints its own loggers;
  - file: `logs/latest.log` as `[HH:mm:ss] [thread/LEVEL] [logger]: message`, rolled to `logs/yyyy-MM-dd-N.log.gz` daily and at startup.
- Name each thread: `main`, and each event loop `IO Worker #N`. Set it with `Thread::setName`, which keeps the whole name for the log.
- Startup and shutdown use Velocity's lines: `Starting {} {}...`, `Connections will use {} channels, {} compression, {} ciphers`, `Listening on {}`, `Done ({}s)!`, `Stopping the proxy...`.
- Where a Velocity word reads oddly, take BDS's: *start* and *stop*, never *boot up* or *shut down*. A server *uses* or *supports* a transport; it never *speaks* or *answers* one.
- A connection names itself as Velocity's `toString` does: `[initial connection] {address}`, `[connected player] {name} ({address})`, `[server connection] {name} -> {server}`. Its lifecycle lines are `{} has connected` and `{} has disconnected`.
- Where Velocity has no counterpart, such as a RakNet or NetherNet event, write the line in the same style.
- Velocity's logging keys sit in `[advanced]` of `endgate.toml` with Velocity's names, defaults and comments, read through `ProxyConfig`:
  - `log-player-connections = true`: log players connecting, switching servers and disconnecting;
  - `log-command-executions = false`: log every command a player runs.
- Keep Velocity's `logger.debug` lines at debug. Debug is off at the default level and comes on with `SPDLOG_LEVEL=debug`.
- Velocity's opt-in diagnostics are keys in a `[debug]` section of `endgate.toml`, keeping Velocity's names and off by default:
  - `packet-decode-logging`: a malformed packet logs its full decode error instead of a one-line notice;
  - `log-server-backpressure`: log when a connection stops or resumes reading because the other side can't keep up.
- Read them through `ProxyConfig` getters, as Velocity reads its settings through `VelocityConfiguration`. Never add environment or command-line switches.
- A malformed packet logs a one-line notice by default: `Invalid packet received. Turn on packet-decode-logging under [debug] in endgate.toml to see more.`
- The integration tests match log lines. Change their patterns in the same commit as the wording.
