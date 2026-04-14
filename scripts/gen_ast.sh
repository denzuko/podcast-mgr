#!/bin/sh
##
# scripts/gen_ast.sh — clang AST dump + filter -> ast.json
#
# Usage:
#   sh scripts/gen_ast.sh            Full: clang dump + filter
#   sh scripts/gen_ast.sh --filter   Filter only (raw file must exist)
#   sh scripts/gen_ast.sh --check    Exit 1 if ast.json is stale
#
# When called from ./nob ast, nob runs the clang step itself and then
# calls this script with --filter for the Python step only.
##

REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO"

RAW="/tmp/podcast_mgr_ast_raw.json"
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

# Step 2: filter via ast_filter.py
case "$MODE" in
    --check)  exec python3 scripts/ast_filter.py --check ;;
    *)        exec python3 scripts/ast_filter.py ;;
esac
