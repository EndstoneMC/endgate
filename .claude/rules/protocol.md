---
paths:
  - "**/*.{h,cpp}"
  - "**/CMakeLists.txt"
---

# Protocol

Every packet endgate reads or writes comes from [EndstoneMC/bedrock-protocol](https://github.com/EndstoneMC/bedrock-protocol). No hand-written packet structs or serializers.

- Build against a release's generated C++ headers, as endweave does:
  - `FetchContent_Declare` the release's `bedrock-protocol-<version>-src.zip`, pinned by its `URL_HASH SHA256`;
  - `FetchContent_MakeAvailable` it;
  - link `bedrock::protocol`.
- The src.zip ships `generated/`, so the build needs neither uv nor bpc. Never run uv or bpc to generate from the schema DSL, and never build against a bedrock-protocol checkout.
- Take the hash from the published release's src.zip.
- A wrong or missing wire format is fixed in bedrock-protocol and released, then endgate bumps its pinned version and hash. Never patch the generated headers or work around them in endgate.
- Include the module you need, `<bedrock/protocol/<module>.h>`.
- `bp::packet_of_t` is `void` for an id whose module isn't included, so a lone module include makes a modelled packet look unmodelled. Code that looks packets up by id either includes `<bedrock/protocol.hpp>`, or includes the modules of the ids it names and `static_assert`s `bp::has_packet_v` for each at every supported version, as `StateRegistry` does. The umbrella costs about 10 s per translation unit, so a header many files include takes the second way.
- A packet whose shape changed between versions is spelled with its protocol, `bp::SubChunkRequestPacket_<975>`. The plain name is only the newest shape.
