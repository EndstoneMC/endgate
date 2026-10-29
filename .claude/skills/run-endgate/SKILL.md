---
name: run-endgate
description: Launches the endgate proxy in front of a backend and drives traffic through it, to see a change working in the real binary rather than in tests alone. Use when asked to run, start or try endgate, to reproduce a bug against a live proxy, or to watch its log while a client connects.
---

# Run endgate

1. Build: `cmake --build --preset conan-relwithdebinfo`.
2. Make a scratch working directory outside the repository. endgate reads `endgate.toml` from its working directory, writes the defaults there when it is missing, and logs to `logs/latest.log` there.
3. Write `endgate.toml`. `online-mode = false` lets clients in without Xbox Live:

   ```toml
   config-version = "1.0"
   bind = "127.0.0.1:19232"
   motd = "Endgate Dev"
   show-max-players = 20
   online-mode = false
   [servers]
   lobby = "127.0.0.1:19233"
   try = ["lobby"]
   [advanced]
   compression-level = -1
   ```

4. Start a backend on the `lobby` address: a Bedrock Dedicated Server, or the echo backend below.
5. From the working directory, start `<repo>/build/RelWithDebInfo/endgate` in the background with stdin from `/dev/null`. It is ready when it logs `Listening on <address>`.
6. Drive it, then stop it with SIGTERM, or with `shutdown` on the console when it runs in a terminal. A clean stop exits 0.

## Echo backend and client

`scripts/echo.py` runs from the uv project in `tests/`:

- `uv run --project tests python -I .claude/skills/run-endgate/scripts/echo.py backend 19233` starts a RakNet server that echoes every message back.
- `uv run --project tests python -I .claude/skills/run-endgate/scripts/echo.py client 127.0.0.1:19232` pings the proxy, connects, and sends messages from 1 byte to 300 KB, each prefixed with `0xFE`, checking every echo.

A Bedrock client joins at the `bind` address once the backend is a real server.

## Symptom → action

| Symptom | Do |
| --- | --- |
| `Unable to read the configuration` or `Your configuration is invalid` | fix `endgate.toml`; a parse error points at the line and column |
| no `Listening on` line | the bind address is taken or malformed |
| `unable to connect to server <name>` | `no NetherNet or RakNet response` means nothing on the backend address supports either transport; `can't resolve the address` means the host name failed to resolve |
| the client's first message gets no reply | endgate holds at most 64 KiB until the backend leg is up, then disconnects the player; send after `[server connection] <player> -> <server> has connected` |
| `nanobind: leaked ...` when a script exits | noise from the raknet binding at interpreter exit |
