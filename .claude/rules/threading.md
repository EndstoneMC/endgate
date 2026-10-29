---
paths:
  - "src/**"
---

# Threading

Netty's event loop model, 1:1, as Velocity uses it: `EventLoopGroup` (`next`, `shutdownGracefully`), `EventLoop` (`inEventLoop`, `execute`, `schedule`), and `Channel::eventLoop()`.

- `ConnectionManager` owns an `EventLoopGroup` with one `EventLoop` per hardware thread, as Velocity's owns its worker group.
- An `EventLoop` is an `asio::io_context` constructed with concurrency hint 1 and run by exactly one thread. `execute` posts to it, `inEventLoop` is `running_in_this_thread`, and `schedule` arms a `steady_timer`.
- A call that may come from another thread checks `inEventLoop()` and runs inline when true, else `execute`s, as Netty's `AbstractChannelHandlerContext` does.
- A player is pinned to one loop for life: its client channel, every backend channel it opens, their engines, timers and session handlers. Velocity opens the backend connection on the player's event loop to save the context switch.
- No strands. Within a loop, one channel hands a batch to the other with a direct call.
- Before binding the group, probe-bind the UDP and TCP port without `SO_REUSEPORT`, so a second instance fails to start. Start accepting only after every endpoint has bound.
- Listen with one UDP socket per loop on the same port with `SO_REUSEPORT`, as Velocity's `enable-reuse-port` binds once per worker. Linux keeps a sender on one socket, so a player is created on the loop that received its first datagram.
- Nothing a player owns is shared across threads, so engines, buffers and session state need no mutex, atomic or lock.
- Server-wide registries may be shared behind a `std::mutex`, as Velocity shares its concurrent maps: the RakNet client GUIDs, NetherNet's pending joins and the loop that owns each NetherNet session.
- Never block a loop. Resolve hostnames, connect and wait asynchronously.
- asio allows concurrent synchronous `send_to` on one socket, but not concurrent asynchronous operations.
- A NetherNet join only writes the answer and records the offer and ICE credentials in the pending-joins table, which expires and is capped. The session is created on the loop whose socket receives the client's first valid STUN check: one whose MESSAGE-INTEGRITY verifies against the pending join's ICE password. Nothing is claimed, forwarded or cached for a check that does not verify.
- A check from another of the client's candidate addresses that lands on a different loop is handed to the owner with `execute()`, and that loop caches which loop owns the address.
