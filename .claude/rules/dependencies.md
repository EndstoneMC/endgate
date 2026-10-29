---
paths:
  - "**/*.{h,cpp}"
  - "**/CMakeLists.txt"
  - "conanfile.txt"
---

# Dependencies

- asio: all socket I/O.
- spdlog: all logging.
- glaze: JSON and TOML.
- replxx: the console reader. Mirror Velocity's JLine-based reader: same behaviour, same responsibilities.
- OpenSSL: DTLS and HMAC.
- libdeflate and snappy: compression.
- Take the design, not the dependency. A framework we would use one idea from is not worth its transitive weight or a second event loop beside asio. Port the idea, name where it came from with `@see`, and pay only for what we use.
