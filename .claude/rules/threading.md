---
paths:
  - "src/**"
---

# Threading

- `ConnectionManager` runs one `asio::io_context` on a worker per hardware thread. Strands serialise our handlers, not the context.
- Engines have no mutex, atomic or lock. The strand that drives an engine is its whole guarantee.
- asio calls a shared socket unsafe for concurrent asynchronous operations; its carve-out covers synchronous calls only.
- Never block a strand. Resolve hostnames, connect and wait asynchronously.

## Strands

On main, an `Endpoint` owns one socket and one strand shared by every connection on it, and everything touching that socket, a session registry or a connection runs there.

Phase 2 replaces that, and these points are planned, not merged:

- The endpoint strand only classifies and routes.
- Each connection gets its own strand and deadline timer, armed at its engines' `nextTimeout()`.
- A connection sends with a synchronous non-blocking `send_to` on the shared socket.
