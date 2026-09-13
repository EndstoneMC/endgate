# CLAUDE.md

Guidance for Claude Code (claude.ai/code) when working in this repository.

Endgate is a modern, high-performance proxy for Minecraft: Bedrock Edition. Its defining goal is seamless transfer between backend servers — no dimension change screen, no transfer packet screen.

## Architecture

- Follow Velocity's structural design. Its module layout, connection lifecycle and event model are the reference — when a structural question comes up, ask how Velocity answers it first.
- We do not copy Velocity's api/core split. There is no separate interface module — one class is both the interface and the implementation.
- Keep Velocity's names. Match class and method names faithfully wherever we reasonably can, so any piece of endgate traces straight back to its Velocity counterpart and to Velocity's own dependencies.
- Server switching is seamless. It uses neither the dimension change packet nor the transfer packet — both put a loading screen in front of the player. Use camera packets to fade the screen out and back in instead.
- The switch happens behind that fade: while the screen is dark, resend everything the client holds per-server — chunks, actors, and the rest of the world state.

## Pipeline

- Packets move through a statically typed handler chain. Each stage declares what it reads and what it emits, so a chain wired wrong fails to build. Netty erases every message to `Object` and finds out at runtime; wangle's `Handler<Rin, Rout, Win, Wout>` is the shape worth copying.
- Stages push, they are never polled. BDS polls because a game server has a tick to drain on, and a proxy has no tick — its only clock is packet arrival, so a poll would add latency to invent one. `fireRead` may be called any number of times for one input, which is how a batch fans out into packets.
- Stages are never inserted or removed. Runtime pipeline surgery is the part of Netty hardest to reconcile with static typing, so a stage that would be removed is present from the start and gated on a flag, the way BDS gates compression and encryption.
- Every chain ends in a sink. Neither Netty nor wangle has one — both log that a message reached the end of the pipeline and drop it. Ours is a typed object, so an unterminated chain is a compile error rather than a warning in production.
- A stage may emit a raw `ByteBuf` or a decoded packet, and the next stage overloads on both. That is how a packet we have no interest in is forwarded without being decoded, which is most of why Velocity is fast.
- `ByteBuf` keeps Netty's name and folly `IOBuf`'s shape, and is ours: segments over shared storage, so a slice copies nothing, a header goes in front without moving the payload, and the chain hands straight to a scatter-gather write.

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
- **RakNet** — vendored, not a Conan dependency. This is the one exception to "no vendored copies": we take RakNet's reliability layer as a *port* we own, not a library we consume. `src/endgate/network/raknet/` holds `ReliabilityLayer`, `CCRakNetSlidingWindow` and `BitStream`, carried over from `raknet/4.081-mojang` in the endstone recipe with all six of its patches applied — including the ACK/NAK range bound, which is a security fix. Re-derive from that recipe, never from the upstream tarball.
  - Upstream filenames are kept so each file traces back to its RakNet counterpart.
  - `RakNetSocket2` and `RakNetRandom` are gone: the layer sends through `endgate::raknet::DatagramSink` (`sink.h`), and the session owns the peer address.
  - `shim.cpp` supplies what RakNet expected from its own runtime — allocators backed by the default allocator, a `std::chrono` clock.
  - `PluginInterface2` is retained deliberately. It is the reliability layer's observability seam: 23 of its 31 call sites report malformed datagrams, which is how the ACK/NAK bound surfaces an attack.

## Approach

- Don't patch the symptom. Work out what the case in front of you is an instance of, and solve that instead.
- Think as an engineer: before writing the change, decide where it belongs in the design. If the same fix would be needed in three places, the abstraction is in the wrong place.
- Generalise. A change that only handles the reported case is unfinished — the next case should already be covered by it.

## C++

- Target C++23. Reach for what the standard gives us — ranges, `std::expected`, `std::span`, `constexpr`, deducing `this` — instead of older idioms or hand-rolled equivalents.
- Follow modern C++ best practice: value semantics, RAII, no raw owning pointers, no manual resource cleanup.
- A `constexpr` used by one function belongs inside it. Lift a constant to namespace scope only once a second function needs it.
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
