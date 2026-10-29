# CLAUDE.md

Endgate is a proxy for Minecraft: Bedrock Edition, built on Velocity's design. Its defining goal is seamless transfer between backend servers, with no dimension change screen and no transfer packet screen.

The rules live in `.claude/rules/`; some load only when matching files are touched. Procedures live in `.claude/skills/`.

## Commands

- Install dependencies: `conan install . -s '&:build_type=RelWithDebInfo' -s compiler.cppstd=23 -c tools.cmake.cmaketoolchain:generator=Ninja --build=missing`
- Configure: `cmake --preset conan-relwithdebinfo`
- Build: `cmake --build --preset conan-relwithdebinfo`
- Unit tests: `ctest --test-dir build/RelWithDebInfo --output-on-failure -j`; one suite with `-R '^Association\.'`
- Integration tests: `uv run --project tests pytest tests/integration`; one test with `-k <name>`. They start the binaries in `build/RelWithDebInfo`, so build first.
- Format: `uvx clang-format@22.1.8 -i <files>`
- Lint: `uvx clang-tidy@22.1.8 -p build/RelWithDebInfo --quiet <files>`

Run `/verify` before committing a change to code or build files.

## Git

- Commit subjects are `type(scope): sentence`, scoped by module (`raknet`, `rtc`, `sctp`, `nethernet`, `network`). The body says why.
- Never add a `Co-Authored-By` line for Claude to commits.
