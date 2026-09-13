# CLAUDE.md

Guidance for Claude Code (claude.ai/code) when working in this repository.

Endgate is a modern, high-performance proxy for Minecraft: Bedrock Edition. Its defining goal is seamless transfer between backend servers — no dimension change screen, no transfer packet screen.

## Architecture

- Follow Velocity's structural design. Its module layout, connection lifecycle and event model are the reference — when a structural question comes up, ask how Velocity answers it first.
- We do not copy Velocity's api/core split. There is no separate interface module — one class is both the interface and the implementation.
- Keep Velocity's names. Match class and method names faithfully wherever we reasonably can, so any piece of endgate traces straight back to its Velocity counterpart and to Velocity's own dependencies.
- Server switching is seamless. It uses neither the dimension change packet nor the transfer packet — both put a loading screen in front of the player. Use camera packets to fade the screen out and back in instead.
- The switch happens behind that fade: while the screen is dark, resend everything the client holds per-server — chunks, actors, and the rest of the world state.

## Build

- CMake with the Ninja generator.
- Conan for every third-party dependency — no vendored copies, no `FetchContent`.

## Dependencies

- **replxx** — the console reader. Mirror Velocity's JLine-based reader: same behaviour, same responsibilities.
- **glaze** — JSON and TOML.
- **spdlog** — all logging.
- **asio** — all socket I/O.
- **libdatachannel** — WebRTC.
- Take the design, not the dependency. A framework we would use one idea from — folly and wangle for `IOBuf` and their Netty-shaped pipeline, say — is not worth its transitive weight or a second event loop beside asio. Port the idea, name where it came from, and pay only for what we use.
- **RakNet** — the RakNet protocol only. Do not use its own socket layer; own the socket with asio and feed RakNet the datagrams.

## Approach

- Don't patch the symptom. Work out what the case in front of you is an instance of, and solve that instead.
- Think as an engineer: before writing the change, decide where it belongs in the design. If the same fix would be needed in three places, the abstraction is in the wrong place.
- Generalise. A change that only handles the reported case is unfinished — the next case should already be covered by it.

## C++

- Target C++23. Reach for what the standard gives us — ranges, `std::expected`, `std::span`, `constexpr`, deducing `this` — instead of older idioms or hand-rolled equivalents.
- Follow modern C++ best practice: value semantics, RAII, no raw owning pointers, no manual resource cleanup.
- Resolve shared services once and hold them. A class that logs takes a `spdlog::logger &` member initialised in its constructor, mirroring Velocity's `private static final Logger logger` — never call `LogManager::getLogger()` from a method that runs more than once. The same goes for anything else looked up by name.
- Best practice is a moving target. These conventions are expected to evolve as the code grows — when a better pattern proves itself, adopt it and record it here.

## Files

- Prefer short file names. Unlike Java, a file need not be named after the class it holds, and one file may hold several related classes.
- Let the directory carry the context: `ProxyVersion` lives in `version.h`, `ProxyServer` in `server.h`, `RollingRandomAccessFileAppender` in `logging/file_appender.h`.

## Comments

- Don't write comments. Make the code explain itself: precise names, small functions, explicit types, early returns.
- If a comment feels necessary, that's a signal to rename or restructure instead.
- Reasoning about *why* a change was made goes in the commit message, never in the source.
- The one exception is the link back to Velocity. A class or method that mirrors a Velocity counterpart may carry a Doxygen docstring naming it, so the correspondence is discoverable from the code. Use endstone's style — JAVADOC_AUTOBRIEF, so no `@brief`:

  ```cpp
  /**
   * Implements the console, including running commands and receiving messages from plugins.
   *
   * @see com.velocitypowered.proxy.console.VelocityConsole
   */
  ```

## Git

- Never add a `Co-Authored-By` line for Claude to commits.
