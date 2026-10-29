#!/bin/sh
file=$(jq -r '.tool_input.file_path // empty')
case "$file" in
"$CLAUDE_PROJECT_DIR"/.claude/*) exit 0 ;;
"$CLAUDE_PROJECT_DIR"/*.h | "$CLAUDE_PROJECT_DIR"/*.cpp) ;;
*) exit 0 ;;
esac
command -v uvx >/dev/null || exit 0
uvx --quiet clang-format@22.1.8 -i "$file" >/dev/null 2>&1 || true
