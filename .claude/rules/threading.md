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
- Listen with one UDP socket per loop on the same port with `SO_REUSEPORT`, as Velocity's `enable-reuse-port` binds once per worker. Linux keeps a sender on one socket, so a player is created on the loop that received its first datagram.
- Nothing a player owns is shared across threads, so engines, buffers and session state need no mutex, atomic or lock.
- Never block a loop. Resolve hostnames, connect and wait asynchronously.
- asio allows concurrent synchronous `send_to` on one socket, but not concurrent asynchronous operations.
- The shared endpoint strand on main and the per-connection strands in the phase 2 endpoint work predate this model: replace them, don't extend them.
- A NetherNet join only writes the answer and records the ICE credentials in a pending-joins table, the one structure shared across loops. The session is created on the loop whose socket receives the client's first valid STUN check.
- A check from another of the client's candidate addresses that lands on a different loop is handed to the owner with `execute()`, and that loop caches which loop owns the address.
