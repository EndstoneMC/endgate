---
paths:
  - "src/**"
---

# Logging

Log what Velocity logs, when it logs it, at the same level. Word each line the way BDS words the same event.

- Name each logger after its class, as Velocity's `LogManager.getLogger(X.class)` does, and hold it as a member.
- The layout is Velocity's `log4j2.xml`:
  - console: `[HH:mm:ss LEVEL]: message` for endgate's own loggers, `[HH:mm:ss LEVEL] [logger]: message` for any other;
  - file: `logs/latest.log` as `[HH:mm:ss] [thread/LEVEL] [logger]: message`, rolled to `logs/yyyy-MM-dd-N.log.gz` daily and at startup.
- Where BDS logs the same event, use its words:
  - startup: `Starting Server`, `Version: {}`, `IPv4 supported, port: {}` or `IPv6 supported, port: {}` per bound socket, `Server started.`;
  - a port already taken: `Network port occupied, can't start server.`;
  - players: `Player connected: {}, xuid: {}` and `Player disconnected: {}, xuid: {}, pfid: {}`;
  - shutdown: `Server stop requested.`, `Stopping server...`, `Quit correctly`.
- For a proxy event BDS never logs, such as a backend connection, a server switch or the channel summary, write it in BDS's style: what happened, a colon, then the facts as `key: value` pairs.
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
