---
paths:
  - "src/endgate/network/**"
---

# Network

## Engines

- `raknet`, `rtc`, `rtc/sctp` and `nethernet` are sans-IO engines: no asio I/O objects, no threads, no locks, no clock reads.
- Time is passed in. An engine reports `nextTimeout()` and is driven by `handleTimeout(now)`.
- Outputs are pushed synchronously to the engine's `Listener` interface.
- Bound every length and count read from the wire before using it.
- No RakNet, libdatachannel, libjuice, usrsctp or dcsctp source goes in the tree, not even adapted. They are references for behaviour; write our own from the RFC or the observed wire format.
- OpenSSL (DTLS, HMAC) is the only protocol dependency. SCTP is ours.
- No hard connection limit. `show-max-players` is only what the ping announces; bound only half-open handshakes and ICE negotiation.
- NetherNet carries no `0xFE` batch prefix. RakNet does.

## Pipeline

- Packets move through a statically typed handler chain. Each stage declares what it reads and emits, so a chain wired wrong fails to build. Netty erases every message to `Object`; ours stays typed, as wangle's `Handler<Rin, Rout, Win, Wout>` does.
- Stages push; they are never polled. A proxy has no tick, and its only clock is packet arrival. Deadlines belong to the transport engines, not the chain.
- `fireRead` may be called any number of times for one input; that is how a batch fans out into packets.
- Never insert or remove a stage at runtime. A stage that would be removed is present from the start and gated on a flag, the way BDS gates compression and encryption.
- Every chain ends in a typed sink, so an unterminated chain is a compile error rather than a warning in production.
- A stage may emit a raw `ByteBuf` or a decoded packet, and the next stage overloads on both. That is how a packet we have no interest in is forwarded without being decoded.
- `ByteBuf` keeps Netty's name and folly `IOBuf`'s shape: segments over shared storage, so a slice copies nothing, a header goes in front without moving the payload, and the chain hands straight to a scatter-gather write.

## Transports

These are phase 2 and planned, not merged:

- One UDP port carries RakNet and NetherNet. NetherNet's HTTP signalling listens on TCP at the same port number.
- `0xFE` is added and stripped only at the RakNet edge.
- A backend's transport is detected: HTTP signalling first, then a RakNet ping. `nethernet://` and `raknet://` only pin it.
