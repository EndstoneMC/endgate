---
paths:
  - "**/CMakeLists.txt"
  - "conanfile.txt"
  - "cmake/**"
---

# Build

- CMake with the Ninja generator.
- Conan for every third-party dependency: no vendored copies, no `FetchContent`.
- Each network module is a static library with an `endgate::<module>` alias and its own `CMakeLists.txt`.
- A module's tests are one executable under `tests/unit/<module>`, registered with `gtest_discover_tests`.
- `tests/integration` runs pytest from the uv project in `tests/`; `tests/interop` holds C++ harnesses the integration suite starts.
