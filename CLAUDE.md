# CLAUDE.md

Guidance for Claude Code (claude.ai/code) when working in this repository.

Endgate is a modern, high-performance proxy for Minecraft: Bedrock Edition. Its defining goal is seamless transfer between backend servers — no dimension change screen, no transfer packet screen.

## Architecture

- Follow Velocity's structural design. Its module layout, connection lifecycle and event model are the reference — when a structural question comes up, ask how Velocity answers it first.
- We do not copy Velocity's api/core split. There is no separate interface module — one class is both the interface and the implementation.
- Keep Velocity's names. Match class and method names faithfully wherever we reasonably can, so any piece of endgate traces straight back to its Velocity counterpart and to Velocity's own dependencies.
- Server switching is seamless. It uses neither the dimension change packet nor the transfer packet — both put a loading screen in front of the player. Use camera packets to fade the screen out and back in instead.
- The switch happens behind that fade: while the screen is dark, resend everything the client holds per-server — chunks, actors, and the rest of the world state.

## Bedrock facts

- Never infer how the game behaves. Anything touching the protocol, the wire format, or BDS's own types is checked against a real source before it is written: `~/bedrock-headers` for BDS's headers, the IDA databases for the bodies those headers omit, `~/endstone/src/bedrock` for reconstructions already cross-checked, `~/protocol-docs` for dumps taken from a running server, `~/bedrock-protocol` for schemas with golden bytes, `~/nethernet` for NetherNet.
- Two independent sources beat one plausible pattern. A header that declares without defining is a gap to take to the disassembly, not one to fill with something that reads correctly.
- When a source is unavailable, say so and leave the question open rather than guessing past it.

## Pipeline

- Packets move through a statically typed handler chain. Each stage declares what it reads and what it emits, so a chain wired wrong fails to build. Netty erases every message to `Object` and finds out at runtime; wangle's `Handler<Rin, Rout, Win, Wout>` is the shape worth copying.
- Stages push, they are never polled. BDS polls because a game server has a tick to drain on, and a proxy has no tick — its only clock is packet arrival, so a poll would add latency to invent one.
- That rule is about the chain, not the transport. A reliability layer has retransmission timers, a delayed-ACK deadline and an idle-timeout deadline, and none of them can be driven by arrival — they exist for when nothing arrives. One `asio::steady_timer` per listener at 10 ms, RakNet's own cadence, is a clock rather than a poll: arrival still drives transmission directly, so the timer adds latency to nothing a peer sends. It has to be this way round — the congestion window is ACK-clocked with no pacer, and the only socket write lives inside `ReliabilityLayer::Update`, so waiting for the tick would cap throughput at cwnd per tick instead of per RTT, and a design that never called `Update` on arrival would be silent rather than merely slow. `fireRead` may be called any number of times for one input, which is how a batch fans out into packets.
- Stages are never inserted or removed. Runtime pipeline surgery is the part of Netty hardest to reconcile with static typing, so a stage that would be removed is present from the start and gated on a flag, the way BDS gates compression and encryption.
- Every chain ends in a sink. Neither Netty nor wangle has one — both log that a message reached the end of the pipeline and drop it. Ours is a typed object, so an unterminated chain is a compile error rather than a warning in production.
- A stage may emit a raw `ByteBuf` or a decoded packet, and the next stage overloads on both. That is how a packet we have no interest in is forwarded without being decoded, which is most of why Velocity is fast.
- `ByteBuf` keeps Netty's name and folly `IOBuf`'s shape, and is ours: segments over shared storage, so a slice copies nothing, a header goes in front without moving the payload, and the chain hands straight to a scatter-gather write.

## Concurrency

- One `Endpoint` is one socket, one `asio::strand`, and every connection that arrived on it. Everything touching a reliability layer, a session registry or that socket runs on the strand. This is not tidiness: `ReliabilityLayer` has no mutex, no atomic and no lock anywhere, and asio's own documentation calls a shared socket unsafe for concurrent asynchronous operations — its carve-out covers synchronous calls only. The strand is the whole guarantee for both.
- The strand is per listener, not per connection, for that second reason: connections on one socket must share one, or two of them can be inside `async_send_to` at once.
- `ConnectionManager` still runs the io_context on several threads; the strand serialises our handlers, not the context.

## Build

- CMake with the Ninja generator.
- Conan for every third-party dependency — no vendored copies, no `FetchContent`.

## Dependencies

- **replxx** — the console reader. Mirror Velocity's JLine-based reader: same behaviour, same responsibilities.
- **glaze** — JSON and TOML.
- **spdlog** — all logging.
- **asio** — all socket I/O.
- Take the design, not the dependency. A framework we would use one idea from — folly and wangle for `IOBuf` and their Netty-shaped pipeline, say — is not worth its transitive weight or a second event loop beside asio. Port the idea, name where it came from, and pay only for what we use.
- **RakNet** — vendored, not a Conan dependency. This is the one exception to "no vendored copies": we take RakNet's reliability layer as a *port* we own, not a library we consume. `src/endgate/network/raknet/` holds `ReliabilityLayer`, `CCRakNetSlidingWindow` and `BitStream`, carried over from `raknet/4.081-mojang` in the endstone recipe with all six of its patches applied — including the ACK/NAK range bound, which is a security fix. Re-derive from that recipe, never from the upstream tarball.
  - Upstream filenames are kept so each file traces back to its RakNet counterpart.
  - `RakNetSocket2` and `RakNetRandom` are gone. The reliability layer calls back into the `RakPeer` that owns it — `SendDatagram` for a datagram it has decided to send, `OnReliabilityLayerNotification` for one it dropped — and the `SystemAddress` upstream already threaded through those signatures says which peer. No interface stands between them: there is exactly one implementor, and a virtual call per datagram buys nothing.
  - Every file keeps its upstream name, including the three we reimplemented: `GetTime.cpp` is a `std::chrono` clock, `RakMemoryOverride.cpp` backs RakNet's allocator hooks with the default allocator, `RakNetTypes.cpp` holds the one `SystemAddress` constructor still needed.
  - The port is formatted to endgate's `.clang-format` like any other source — it is code we own, not a dependency we track. clang-tidy stays off for the folder (`.clang-tidy` with `Checks: '-*'`): its checks would demand redesign, not formatting, and the point of the port is to keep RakNet's proven logic intact.
  - `PluginInterface2` is not in the tree and never was. `OnReliabilityLayerNotification` is what it collapsed into: 22 of the layer's 23 calls into `RakPeer` are reports rather than sends, and they are the only surface the ACK/NAK range bound has, because `HandleSocketReceiveFromConnectedPlayer`'s return value is discarded by every caller.
  - `RakPeer` owns no thread, no socket and no receive queue. `ConnectionManager` calls `RunUpdateCycle` — upstream's own `RAKPEER_USER_THREADED` contract — and `Endpoint` owns the socket. Of the twelve places upstream queued a `Packet` for a caller to drain, one carried a user message; it fires `channelRead`, and the rest are channel lifecycle or unconnected traffic endgate answers itself.

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
