---
paths:
  - "**/CMakeLists.txt"
  - "conanfile.txt"
  - "cmake/**"
  - ".github/**"
---

# Build

- Build with each platform's default toolchain: MSVC on Windows, GCC with libstdc++ on Linux, Apple clang on macOS. Never force a compiler in CI or a Conan profile, and never export `CC` or `CXX`.
- Where one compiler lacks a builtin or flag, the source takes that compiler's own path; the build never swaps the compiler to avoid it.
- Endgate is independent of BDS and needs no ABI compatibility with it, unlike Endstone, so it takes none of Endstone's toolchain choices.
- CMake with the Ninja generator.
- Conan for every third-party dependency: no vendored copies, no `FetchContent`.
- Each network module is a static library with an `endgate::<module>` alias and its own `CMakeLists.txt`.
- A module's tests are one executable under `tests/unit/<module>`, registered with `gtest_discover_tests`.
- `tests/integration` runs pytest from the uv project in `tests/`; `tests/interop` holds C++ harnesses the integration suite starts.
