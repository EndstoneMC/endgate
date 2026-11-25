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
- After bumping bedrock-protocol, check the new protocol's `ActorDataIDs` for added keys, and add each key whose value is an actor id to `ACTOR_ID_KEYS` in `src/endgate/protocol/entity.cpp`.
- Include the module you need, `<bedrock/protocol/<module>.h>`.
- Code that looks packets up by id includes `<bedrock/protocol/packets_fwd.hpp>`, as `StateRegistry` does. It maps every modelled id to its type at each protocol but only declares the types, so code that uses a looked-up packet also includes its module.
- A packet whose shape changed between versions is spelled with its protocol, `bp::SubChunkRequestPacket_<975>`. The plain name is only the newest shape.
