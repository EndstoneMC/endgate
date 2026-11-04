---
paths:
  - "**/*.{h,cpp}"
---

# C++

- Target C++23. Reach for ranges, `std::expected`, `std::span`, `constexpr` and deducing `this` instead of older idioms or hand-rolled equivalents.
- Value semantics and RAII: no raw owning pointers, no manual resource cleanup.
- Never downcast one of endgate's own types with `dynamic_cast` or `static_cast`, even where a check before it makes the cast safe. The base of a hierarchy narrowed at runtime carries Endstone's `is<T>()` and `as<T>()`, simplified: a virtual `isInstanceOf(const std::type_info &)` each subclass answers, as `CommandSource` does. Lift them into a shared `Object` base only once a second hierarchy needs them. Exceptions are standard types and keep their `dynamic_cast`.
- Create a shared object with `std::make_shared`, never `std::shared_ptr<T>(new T(...))`. When the constructor must stay private, use a passkey: a public constructor that takes a private tag type only the class can make.
- A `constexpr` used by one function belongs inside it. Lift it to namespace scope once a second function needs it.
- Resolve shared services once and hold them. A class that logs takes a `spdlog::logger &` member initialised in its constructor, mirroring Velocity's `private static final Logger logger`. A copyable value type, where a reference member would break assignment, resolves its logger once through a private static `logger()` instead. Never initialise a logger at namespace or static-member scope, which runs before `main`. Never look a logger, or anything else, up by name in a method that runs more than once.
- `#include <...>` for the standard library and third-party headers, `"endgate/..."` for ours.
- Coroutines return `Awaitable<T>` (`endgate/awaitable.h`), never `asio::awaitable<T>` directly.
- Spell atomics `std::atomic<T>`, not the `std::atomic_bool` family of aliases.
- Initialise a member with `=`, never braces: `bool active_ = true;`, `int count_ = 0;`. A member whose default constructor gives what it needs takes no initialiser.
- A `glz::object` list puts each key and its member on a line of their own, ended with an empty `//`, so clang-format keeps one pair per line: `"read-timeout", &T::read_timeout,  //`. The last pair closes the call instead. This line-break marker is the one empty comment allowed.
- `.clang-tidy` owns naming and runs with warnings as errors. Run it rather than guess; don't silence it with `NOLINT`.
- When a better pattern proves itself, adopt it and update this file.

## Files

- Prefer short file names. A file need not be named after its class, and one file may hold several related classes.
- Let the directory carry the context: `ProxyServer` lives in `server.h`, `RollingRandomAccessFileAppender` in `logging/appender.h`.
- A one-line alias or constant gets no file of its own. Declare it where it's used, inside the function when one function uses it.

## Comments

- Don't write comments. Make the code explain itself with precise names, small functions, explicit types and early returns. A comment that feels necessary is a signal to rename or restructure.
- Reasoning about why a change was made goes in the commit message, never in the source.
- Docstrings are the one exception; see `docstring.md`.
