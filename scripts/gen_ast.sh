#!/bin/sh
##
# scripts/gen_ast.sh — clang AST dump + jq filter -> ast.json
#
# Usage:
#   sh scripts/gen_ast.sh            Full: clang dump + jq filter
#   sh scripts/gen_ast.sh --filter   jq filter only (raw file must exist)
#   sh scripts/gen_ast.sh --check    Exit 1 if ast.json is stale
#
# Dependencies: clang, jq
# No Python required.
##

REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO"

RAW="/tmp/podcast_mgr_ast_raw.json"
OUT="ast.json"
MODE="${1:-full}"

# Step 1: clang dump (skipped in --filter / --check modes)
if [ "$MODE" = "full" ]; then
    command -v clang > /dev/null 2>&1 || { echo "Error: clang not found" >&2; exit 1; }
    clang -Xclang -ast-dump=json \
          -fsyntax-only -fno-color-diagnostics \
          -w -ferror-limit=0 \
          -D_GNU_SOURCE -I. main.c > "$RAW" 2>/dev/null || true
    [ -s "$RAW" ] || { echo "Error: clang produced no output" >&2; exit 1; }
fi

command -v jq > /dev/null 2>&1 || { echo "Error: jq not found" >&2; exit 1; }

[ -f "$RAW" ] || { echo "Error: $RAW not found — run without --filter first" >&2; exit 1; }

# Step 2: jq filter
if [ "$MODE" = "--check" ]; then
    NEW="$(jq -f scripts/ast_filter.jq "$RAW")"
    EXISTING="$(cat "$OUT" 2>/dev/null || echo '')"
    if [ "$NEW" != "$EXISTING" ]; then
        echo "$OUT is stale — run: sh scripts/gen_ast.sh" >&2
        exit 1
    fi
    echo "$OUT is up to date"
else
    jq -f scripts/ast_filter.jq "$RAW" > "$OUT"
    echo "$OUT written"
fi
