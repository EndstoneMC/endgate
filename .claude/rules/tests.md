---
paths:
  - "tests/**"
---

# Tests

- Don't test that a packet decodes and encodes. bedrock-protocol's own tests cover every packet against golden bytes; endgate's tests cover what endgate does with the packets.
- On Windows the harness stops endgate with `shutdown` on its stdin, since `send_signal(SIGTERM)` is `TerminateProcess` and exits 1; it pipes stdin for every proxy there.
- On Windows aioice gathers on LAN addresses, and a UDP socket on one cannot send to 127.0.0.1 (WinError 1214), so the harness gathers on the loopback only.
- A path written into `endgate.toml` goes through `as_posix()`; backslashes are escapes in a TOML basic string.
- Binary defaults take `EXE` (`.exe` on Windows); the `ENDGATE_BINARY`, `ENDGATE_BEDROCK_PEER`, `ENDGATE_NETHERNET_ECHO` and `ENDGATE_NETHERNET_DIAL` variables override each path.
