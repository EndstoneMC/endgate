---
name: port-from-velocity
description: Ports a Velocity class or subsystem into endgate, keeping Velocity's names and structure and collapsing its api/proxy split into one C++ class. Use when adding a proxy feature Velocity already has, such as commands, events, the connection lifecycle, server switching, configuration or the console, or when asked to port, mirror or follow Velocity.
---

# Port from Velocity

Velocity is PaperMC/Velocity. Read the Java before writing any C++.

1. Find both counterparts: the interface under `api/src/main/java/com/velocitypowered/api/` and its implementation under `proxy/src/main/java/com/velocitypowered/proxy/`. Read the call sites that drive their lifecycle too.
2. Make one class. Name it after the API interface when there is one (`VelocityServer` implements `ProxyServer`, so ours is `ProxyServer`), otherwise drop the `Velocity` prefix (`VelocityConsole` becomes `Console`).
3. Keep the method names, getters included (`getConfiguration`, `getConsoleCommandSource`).
4. Put it in the directory of its subsystem with a short file name: `ProxyServer` in `server.h`, `ConnectionManager` in `network/manager.h`, `ServerListPingHandler` in `network/ping.h`.
5. Translate Velocity's dependencies into ours rather than porting them:

   | Velocity | endgate |
   | --- | --- |
   | Netty event loops, channels | asio, strands (`threading.md`) |
   | Netty pipeline and `ByteBuf` | `network/pipeline.h`, `network/codec.h`, `buffer/bytebuf.h` |
   | `private static final Logger` | a `spdlog::logger &` member |
   | JLine, TerminalConsoleAppender | replxx, `logging/appender.h` |
   | night-config | glaze |

6. Write the docstring from Velocity's javadoc, adapted to endgate, with `@see` on the interface then the implementation (`docstring.md`).
7. A setting goes in `resources/endgate.toml` under Velocity's key, with velocity.toml's comment adapted.
8. Velocity speaks Java Edition. Its structure carries over; its packets and states do not. Take every packet, state and wire detail from the Bedrock sources in `bedrock.md`.
9. Run `/verify`.
