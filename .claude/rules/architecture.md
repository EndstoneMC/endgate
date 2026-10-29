# Architecture

- Follow Velocity's structural design: its module layout, connection lifecycle and event model. Settle a structural question by asking how Velocity answers it first.
- Endgate reads like Velocity on Netty, not like BDS. Take Netty's and Velocity's names and API shapes; BDS is a source of facts about Bedrock, not of design.
- Where endgate implements a Netty concept (`ByteBuf`, the channel pipeline, event loops), match Netty's API 1:1: class names, method names, signatures and semantics. Deviate only where C++ forces it or performance demands it, and list each deviation in the rule that covers it.
- Java's standard types map to their C++ standard counterparts (`IndexOutOfBoundsException` to `std::out_of_range`, `byte[]` to `std::span<std::byte>`).
- Don't take wangle's or folly's design unless it buys performance that Netty's design can't.
- No api/core split. One class is both the interface and the implementation.
- Keep Velocity's class and method names wherever we reasonably can, so every piece of endgate traces back to its Velocity counterpart and to Velocity's own dependencies.
- Server switching is seamless. Never use the dimension change or transfer packet; both put a loading screen in front of the player. Fade the screen out and back in with camera packets instead.
- While the screen is dark, resend everything the client holds per server: chunks, actors and the rest of the world state.

## Approach

- Don't patch the symptom. Work out what the case in front of you is an instance of, and solve that.
- Decide where a change belongs in the design before writing it. If the same fix would be needed in three places, the abstraction is in the wrong place.
- A change that only handles the reported case is unfinished. The next case should already be covered by it.
