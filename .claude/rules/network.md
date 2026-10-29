---
paths:
  - "src/endgate/network/**"
  - "src/endgate/connection/**"
---

# Network

## Engines

- The protocol engines in `raknet`, `rtc`, `rtc/sctp` and `nethernet` are sans-IO: no asio I/O objects, no threads, no locks, no clock reads. NetherNet's HTTP signalling server and client (`signaling`, `client`) are the I/O around them and follow `threading.md`.
- Engines read with `ByteBufView` and write with `ByteBuf`. No private reader, writer or byte-vector plumbing.
- Time is passed in. An engine reports `nextTimeout()` and is driven by `handleTimeout(now)`.
- Outputs are pushed synchronously to the engine's `Listener` interface. A received message arrives as a `ByteBufView`; an engine copies it into a `ByteBuf` only when it must keep it.
- Bound every length and count read from the wire before using it.
- No RakNet, libdatachannel, libjuice, usrsctp or dcsctp source goes in the tree, not even adapted. They are references for behaviour; write our own from the RFC or the observed wire format.
- OpenSSL (DTLS, HMAC) is the only protocol dependency. SCTP is ours.
- No hard connection limit. `show-max-players` is only what the ping announces; bound only half-open handshakes and ICE negotiation.
- NetherNet carries no `0xFE` batch prefix. RakNet does.

## ByteBuf

Netty's `io.netty.buffer.ByteBuf`, 1:1: `readerIndex`, `writerIndex`, `capacity`, `readableBytes`, `writableBytes`, `isReadable`, `ensureWritable`, `skipBytes`, `discardReadBytes`, `clear`, the `read`, `get`, `set` and `write` families with their `LE` and `Unsigned` forms, `readSlice`, `slice`, `copy`, `nioBuffer`. Netty's growth rule (`calculateNewCapacity`) and `IndexOutOfBoundsException` (as `std::out_of_range`) come with it.

- Netty's integers are big-endian; Bedrock's are mostly little-endian, so reach for the `LE` methods.
- Bedrock's varints and strings are free functions in `ProtocolUtils` over a `ByteBuf` or `ByteBufView`, as Velocity's are. Every length they read has a cap.
- Allocate through `ByteBufAllocator` (`ctx.alloc()`), so a pooled allocator can replace the unpooled one without touching callers.
- `buffer.h`'s chained, shared `ByteBuf` predates these rules: replace it, don't extend it.

Deviations from Netty:

- Ownership is RAII and moves, not `ReferenceCounted`: no `retain`, `release`, `refCnt`, `retainedSlice` or `retainedDuplicate`. A `ByteBuf` is move-only; where Netty would retain, `copy()`.
- `slice()`, `readSlice()` and `duplicate()` return a `ByteBufView`: borrowed, read-only bytes with a reader index of their own, which is Netty's unretained slice with the borrow in its type. Store a `ByteBuf`, never a `ByteBufView`; growing a `ByteBuf` invalidates every view into it.
- Write the read surface once for `ByteBuf` and `ByteBufView` with deducing `this`, not virtuals.
- Memory is never zero-filled. A socket, decompressor or compressor writes straight into the writable region after `ensureWritable(n)`, then advances `writerIndex`.
- No `CompositeByteBuf`. Compressors, ciphers and datagram assembly all want contiguous bytes.

## Pipeline

Netty's channel pipeline, 1:1: `ChannelPipeline`, `ChannelHandlerContext`, `ChannelInboundHandler` (`channelActive`, `channelInactive`, `channelRead`, `channelReadComplete`, `exceptionCaught`, `userEventTriggered`), `ChannelOutboundHandler` (`write`, `flush`, `close`), `fireChannelRead` and the other `fire` methods, `writeAndFlush`, and the codec bases `ByteToMessageDecoder`, `MessageToMessageDecoder` and `MessageToByteEncoder`.

- Handlers are Velocity's, in Bedrock's order. Inbound: strip `0xFE` (RakNet only), `MinecraftCipherDecoder`, `MinecraftCompressDecoder`, `MinecraftVarintFrameDecoder`, `MinecraftDecoder`, then `MinecraftConnection`, which hands packets to the `MinecraftSessionHandler`. Outbound: `MinecraftEncoder`, `MinecraftVarintLengthEncoder`, then on `flush` a compression encoder and `MinecraftCipherEncoder`.
- Bedrock compresses the whole batch, so compression sits outside framing, the reverse of Java Edition.
- `MinecraftDecoder` decodes only the packets the session handler registered for, and passes the rest on as a `ByteBufView` to `handleUnknown`, where Velocity passes `buf.retain()`.
- Decoded packets own their fields, as Velocity's do. Forwarding never decodes.
- Flush at the end of each inbound batch, in `channelReadComplete`, so a forwarded batch leaves with the packets it arrived with.
- Forward an unchanged batch whose two legs share a compression setting as the compressed bytes that arrived.
- Stages push; nothing polls. Deadlines belong to the transport engines, not the pipeline.

Deviations from Netty, all for performance:

- The pipeline is statically typed. Each handler declares the message types it reads and writes, so a chain wired wrong fails to build, and no message is erased to `Object` or dispatched through a virtual call.
- Handlers are never added or removed at runtime. One that Netty would add later (a cipher, a compressor) is present from the start and gated on a flag, the way BDS gates compression and encryption.
- A decoder calls `fireChannelRead` for each message it produces, instead of filling an `out` list.
- `write` returns nothing, as if every write passed Netty's `voidPromise()`. Errors go to `exceptionCaught`.
- Every chain ends in a typed sink, so an unterminated chain is a compile error rather than Netty's "discarded inbound message" warning.

## Transports

These are phase 2 and planned, not merged:

- One UDP port carries RakNet and NetherNet. NetherNet's HTTP signalling listens on TCP at the same port number.
- `0xFE` is added and stripped only at the RakNet edge.
- A backend's transport is detected: HTTP signalling first, then a RakNet ping. `nethernet://` and `raknet://` only pin it.
