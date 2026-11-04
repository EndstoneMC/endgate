---
paths:
  - "src/endgate/network/**"
  - "src/endgate/protocol/**"
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
- No hard connection limit. `show-max-players` is only what the ping announces; bound only half-open handshakes (1024 for the whole server) and ICE negotiation (pending joins, 1024).
- NetherNet carries no `0xFE` batch prefix. RakNet does.
- A NetherNet client's offer must carry an `a=identity` that passes BDS's check whatever `online-mode` says, or it is refused with `ConnectError` 37 (`eSessionErrorIdentityNotAllowed`). The check is `ClientIdentityAssertionVerifier::verify`: the assertion's `token` is a three-part JWT of at most 16 KiB with a string `cpk` (base64 DER), and its `fingerprints` field is a detached JWS over the offer's `a=fingerprint` set that verifies under that `cpk`.
- With `online-mode = true`, the offer token is also validated the way BDS validates a login token (`GameServerToken::validate`): Minecraft Services' key for its `kid`, the multiplayer `aud`, expiry. At login, the login token's `xid` and `cpk` must equal the offer token's, or the player is disconnected.

## ByteBuf

Netty's `io.netty.buffer.ByteBuf`, 1:1: `readerIndex`, `writerIndex`, `capacity`, `readableBytes`, `writableBytes`, `isReadable`, `ensureWritable`, `skipBytes`, `discardReadBytes`, `clear`, the `read`, `get`, `set` and `write` families with their `LE` and `Unsigned` forms, `readSlice`, `slice`, `copy`, `nioBuffer`. Netty's growth rule (`calculateNewCapacity`) and `IndexOutOfBoundsException` (as `std::out_of_range`) come with it.

- Netty's integers are big-endian; Bedrock's are mostly little-endian, so reach for the `LE` methods.
- One `ByteBuf` call per statement. Netty's methods return the buffer for chaining; endgate never chains them.
- Allocate with `Unpooled`, Netty's static factory.

Deviations from Netty:

- Ownership is RAII and moves, not `ReferenceCounted`: no `retain`, `release`, `refCnt`, `retainedSlice` or `retainedDuplicate`. A `ByteBuf` is move-only; where Netty would retain, `copy()`.
- `slice()`, `readSlice()` and `duplicate()` return a `ByteBufView`: borrowed, read-only bytes with a reader index of their own, which is Netty's unretained slice with the borrow in its type. Store a `ByteBuf`, never a `ByteBufView`; growing a `ByteBuf` invalidates every view into it.
- Write the read surface once for `ByteBuf` and `ByteBufView` with deducing `this`, not virtuals.
- Bedrock's encodings are methods on the buffers themselves, because we own `ByteBuf`: `readVarInt`/`writeVarInt` (zigzag), `readUnsignedVarInt`/`writeUnsignedVarInt`, their 64-bit forms `readVarInt64`/`writeVarInt64` and `readUnsignedVarInt64`/`writeUnsignedVarInt64`, and `readString(maxLength)`/`writeString`. There is no `ProtocolUtils`. Every length they read has a cap.
- Memory is never zero-filled. A socket, decompressor or compressor writes straight into the writable region after `ensureWritable(n)`, then advances `writerIndex`.
- No `CompositeByteBuf`. Compressors, ciphers and datagram assembly all want contiguous bytes.
- `getBytes` returns nothing instead of the buffer, since returning the const reference trips clang-tidy.

## Pipeline

Netty's channel pipeline, 1:1: `ChannelPipeline`, `ChannelHandlerContext`, `ChannelInboundHandler` (`channelActive`, `channelInactive`, `channelRead`, `channelReadComplete`, `exceptionCaught`, `userEventTriggered`), `ChannelOutboundHandler` (`write`, `flush`, `close`), `fireChannelRead` and the other `fire` methods, `writeAndFlush`, `ChannelFuture` and `ChannelPromise` with `addListener` and `ChannelFutureListener::CLOSE`, and the codec bases `ByteToMessageDecoder`, `MessageToMessageDecoder` and `MessageToByteEncoder`.

- Handlers are Velocity's, in Bedrock's order. Inbound: strip `0xFE` (RakNet only), `MinecraftCipherDecoder`, `MinecraftCompressDecoder`, `MinecraftVarintFrameDecoder`, `ReadTimeoutHandler`, `EntityRewriteHandler`, `MinecraftDecoder`, then `MinecraftConnection`, which hands packets to the `MinecraftSessionHandler`. Outbound: `MinecraftEncoder`, `EntityRewriteHandler`, `MinecraftVarintLengthEncoder`, then on `flush` a compression encoder and `MinecraftCipherEncoder`.
- Bedrock compresses the whole batch, so compression sits outside framing, the reverse of Java Edition.
- Packets are registered as Velocity's `StateRegistry` registers them: each state lists what it decodes in each direction (`PacketRegistries<State>`), and each supported version resolves that list to bedrock-protocol's types (`ProtocolRegistry<Version>`). `MinecraftDecoder` takes Velocity's direction, `setState` and `setProtocolVersion`, and passes a packet its registry lacks on as a `ByteBufView` to `handleUnknown`, where Velocity passes `buf.retain()`.
- A registered packet reaches the handler's `handle` overload for its type. One no overload takes reaches `handleGeneric` as the bytes it arrived in, which by default go to `handleUnknown`.
- Bedrock redeclares a packet whenever its wire form changes, so one id has several types across the supported versions. `PacketTypes<Id>` names them, and a handler that treats them alike takes them all through `HandleEach`'s `handleEach` template instead of an override each.
- Decoded packets own their fields, as Velocity's do. Forwarding never decodes, except on a backend connection whose `EntityMap` isn't the identity: there `EntityRewriteHandler` decodes the packets that carry actor IDs to swap the player's runtime and unique IDs.
- Flush at the end of each inbound batch, in `channelReadComplete`, so a forwarded batch leaves with the packets it arrived with.
- Every forwarded batch is decompressed and compressed again, as Velocity does with every packet it forwards.
- Stages push; nothing polls. Deadlines belong to the transport engines, not the pipeline.

Deviations from Netty, all for performance:

- The pipeline is statically typed. Each handler declares the message types it reads and writes, so a chain wired wrong fails to build, and no message is erased to `Object` or dispatched through a virtual call.
- Handlers are never added or removed at runtime. One that Netty would add later (a cipher, a compressor) is present from the start and gated on a flag, the way BDS gates compression and encryption.
- A decoder calls `fireChannelRead` for each message it produces, instead of filling an `out` list.
- `write` returns nothing, as if every write passed Netty's `voidPromise()`. Errors go to `exceptionCaught`. Only `MinecraftConnection::writeAndFlush` returns a `ChannelFuture`.
- Every chain ends in a typed sink, so an unterminated chain is a compile error rather than Netty's "discarded inbound message" warning.
- `MinecraftDecoder` fires a `DecodedPacket` that keeps the bytes the packet arrived in, so a packet no handler changes is handed on as those bytes instead of being encoded again.
- `MinecraftEncoder` has no state: a Bedrock packet's id is the same in every state, so it encodes any packet of the connection's version and refuses one of another.

## Transports

- One UDP port carries RakNet and NetherNet. NetherNet's HTTP signalling listens on TCP at the same port number.
- The default `bind` is `[::]:19132`, one dual-stack socket for IPv6 and IPv4, where Velocity's is `0.0.0.0`. A Bedrock client tries a name's IPv6 address first, so behind an IPv4-only bind a join to a name such as `localhost` waits for that attempt to time out; BDS listens on IPv6 too (`server-portv6`). A machine without IPv6 falls back to `0.0.0.0` with a warning.
- `0xFE` is added and stripped only at the RakNet edge.
- Closing after a message is Velocity's `closeWith`: `writeAndFlush(message).addListener(ChannelFutureListener::CLOSE)`. Never close on a grace timer or a deadline.
- The transport completes a write's `ChannelPromise` once a close can no longer lose the bytes, which a TCP socket's kernel guarantees for Netty. RakNet completes it on `send`, since its graceful close (`DisconnectAsap`) delivers queued reliable messages first. NetherNet has no kernel behind it, so it completes the promise once SCTP has every chunk acknowledged (`Sender::idle()`).
- A backend's transport is detected: HTTP signalling first, then a RakNet ping. NetherNet goes first because most backends run it. `nethernet://` and `raknet://` only pin it.
- A BDS backend answers only its own transport. A `transport=nethernet` BDS answers no RakNet ping, and a RakNet BDS refuses `/v1/join`'s TCP connect, which Windows takes about 2 s to report, so detecting a RakNet backend costs those 2 s once. Checked on 1.26.60.
- BDS fills `GET /v1/join` with the ping JSON (`name`, `protocol`, `version`, `level`, `players`, `maxPlayers`, `gameType`) only when `enable-lan-visibility=true`. With it `false`, the answer is `200 application/json` with `Content-Length: 0`. No header, query or method changes that. Checked on 1.26.60 (`getMetadata` is empty when its callback source is null, `sub_141119D00`).
- The same switch governs RakNet: with `enable-lan-visibility=false`, BDS answers an unconnected ping with a pong whose payload is empty, so it carries no protocol either. Checked on 1.26.52.
- `ping-passthrough` has no `mods` mode. Velocity's MODS copies the backend's Forge mod list, which Bedrock doesn't have. A passed-through ping asks every candidate backend at once and takes the first answer in list order, as Velocity does.
- A backend that answers an empty `/v1/join` still speaks NetherNet but has no protocol. The protocol is not available without a login then, since BDS binds no UDP 7551 either.
- With `enable-lan-visibility=true`, UDP 7551 answers an encrypted LAN discovery request with the same data as `ServerData` hex (see `nethernet-python` `discovery/`). It is the same switch, so it is no fallback.
- BDS does not answer GameSpy 4 query: its 1.26.51 Linux binary holds no `enable-query` or `query-port` property and none of the protocol's strings (`splitnum`, `hostport`, `numplayers`, `game_id`), and bedrock-headers has no query listener. Query is endgate's own, so it is not a pass-through.
- A datagram that starts `FE FD` from a sender with no RakNet route is query's. No RakNet datagram starts there (a data datagram's flags top out at `0x9C`, an ACK's at `0xE0`, a NAK's at `0xA0`), nor does a NetherNet one (STUN opens with `0x00`-`0x03`, DTLS with `0x14`-`0x3F`). With the query port equal to the bind port the endpoint hands such a datagram to the handler before RakNet or NetherNet sees it; any other port gets a socket of its own on one loop.
