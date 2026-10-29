---
name: verify
description: Builds endgate and proves a change works with unit tests, the integration suite against the real binaries, clang-format and clang-tidy. Use before every commit that touches code or build files, after a refactor that must not change behaviour, or when asked to verify, check or test a change.
---

# Verify

Run every step from the repository root, and report each command with its result. On a failure, fix it and start again from step 1.

1. Build: `cmake --build --preset conan-relwithdebinfo`. If `build/RelWithDebInfo` is missing, or `conanfile.txt` changed, run the install and configure commands from CLAUDE.md first.
2. Unit tests: `ctest --test-dir build/RelWithDebInfo --output-on-failure -j`
3. Integration tests: `uv run --project tests pytest tests/integration -q`
4. Format: `git ls-files --cached --others --exclude-standard -- '*.h' '*.cpp' | xargs uvx clang-format@22.1.8 --dry-run --Werror`
5. Lint the sources you touched: `{ git diff --name-only --diff-filter=d HEAD; git ls-files --others --exclude-standard; } | grep '\.cpp$' | xargs -r uvx clang-tidy@22.1.8 -p build/RelWithDebInfo --quiet`. A touched header is checked through a source that includes it, so add one when only headers changed.

## Symptom → action

| Symptom | Do |
| --- | --- |
| `endgate binary not found` or `endgate_nethernet_echo not found` | build first; both come from step 1 |
| `endgate did not start listening` | read the log it prints: a config parse error or a bind failure stops startup |
| `endgate exited during the test` or `did not shut down cleanly` | a crash or a non-zero exit on SIGTERM; the last 40 log lines are in the failure |
| the NetherNet suite waits forever at start | the echo harness's `listening <port>` line is stuck in a pipe buffer; the harness must flush it |
| uv warns that `VIRTUAL_ENV` does not match | harmless; the suite runs in `tests/.venv` |
| clang-tidy flags a name | rename it as `.clang-tidy` asks; don't add `NOLINT` |
