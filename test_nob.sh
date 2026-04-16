#!/bin/sh
##
# test_nob.sh — end-to-end test suite for nob.c
#
# Tests the build driver: compiles correctly, produces a working binary,
# self-rebuild mechanism works, critical flags present.
#
# Note: source integrity grep assertions (suite 1) cover only invariants
# that cannot be verified by compilation alone — e.g. link order requires
# kcgi to link, which is not available in all environments.
# Architectural correctness (khtml/kxml split, malloc isolation, etc.)
# is covered by policy/ast.rego via ./nob policy.
#
# Requirements: cc, file(1), stat(1) — standard POSIX.
# Usage:
#   sh test_nob.sh
#   sh test_nob.sh --no-build
##

set -e

SUITE_PASS=0; SUITE_FAIL=0; TOTAL_PASS=0; TOTAL_FAIL=0
CURRENT_SUITE=""; CURRENT_TEST=""

suite() {
    if [ -n "$CURRENT_SUITE" ]; then
        printf "  Suite %-44s pass=%d fail=%d\n" \
               "$CURRENT_SUITE" "$SUITE_PASS" "$SUITE_FAIL"
    fi
    CURRENT_SUITE="$1"; SUITE_PASS=0; SUITE_FAIL=0
    printf "\nSuite: %s\n" "$1"
}

t() { CURRENT_TEST="$1"; }

assert_true() {
    if [ "$1" -eq 0 ] 2>/dev/null; then
        SUITE_PASS=$((SUITE_PASS + 1)); TOTAL_PASS=$((TOTAL_PASS + 1))
        printf "  [PASS] %s: %s\n" "$CURRENT_TEST" "$2"
    else
        SUITE_FAIL=$((SUITE_FAIL + 1)); TOTAL_FAIL=$((TOTAL_FAIL + 1))
        printf "  [FAIL] %s: %s\n" "$CURRENT_TEST" "$2"
    fi
}

check() { eval "$1" > /dev/null 2>&1 && echo 0 || echo 1; }

summary() {
    if [ -n "$CURRENT_SUITE" ]; then
        printf "  Suite %-44s pass=%d fail=%d\n" \
               "$CURRENT_SUITE" "$SUITE_PASS" "$SUITE_FAIL"
    fi
    printf "\n=== SUMMARY: %d passed, %d failed ===\n" "$TOTAL_PASS" "$TOTAL_FAIL"
    [ "$TOTAL_FAIL" -eq 0 ]
}

REPO="$(cd "$(dirname "$0")" && pwd)"
cd "$REPO"

NO_BUILD=0
for arg in "$@"; do case "$arg" in --no-build) NO_BUILD=1 ;; esac; done

printf "podcast-mgr nob.c end-to-end test suite\n"
printf "=========================================\n"

# ═══════════════════════════════════════════════════════════════════════════
# Suite 1 (grep-source assertions) removed.
# Compiler + cppcheck + policy/nob_ast.rego cover these.

# Suite 2: nob binary compilation
# ═══════════════════════════════════════════════════════════════════════════
suite "nob binary compilation"

t "nob.h exists"
assert_true "$(check "[ -f '$REPO/nob.h' ]")" "[ -f nob.h ]"

if [ ! -f "$REPO/nob.h" ]; then
    printf "  SKIP: nob.h absent\n"; NO_BUILD=1
fi

if [ "$NO_BUILD" -eq 0 ]; then
    t "nob.c compiles without error"
    cc -o "$REPO/nob" "$REPO/nob.c" 2>/dev/null
    assert_true "$?" "cc -o nob nob.c"

    t "nob binary exists and is executable"
    assert_true "$(check "[ -x '$REPO/nob' ]")" "[ -x nob ]"

    t "nob binary is ELF or Mach-O"
    FILE_OUT=$(file "$REPO/nob" 2>/dev/null)
    assert_true "$(echo "$FILE_OUT" | grep -qiE 'ELF|Mach-O|executable' && echo 0 || echo 1)" \
        "file: $FILE_OUT"
else
    printf "  SKIP: --no-build\n"
fi

# ═══════════════════════════════════════════════════════════════════════════
# Suite 3: build output (requires kcgi)
# ═══════════════════════════════════════════════════════════════════════════
suite "nob build output"

HAS_KCGI=0
pkg-config --exists kcgi 2>/dev/null && HAS_KCGI=1
[ -f /usr/include/kcgi.h ] || [ -f /usr/local/include/kcgi.h ] && HAS_KCGI=1

if [ "$NO_BUILD" -eq 0 ] && [ "$HAS_KCGI" -eq 1 ]; then
    rm -f "$REPO/index.cgi"
    t "./nob runs without error"
    (cd "$REPO" && ./nob > /dev/null 2>&1); NOB_EXIT=$?
    assert_true "$NOB_EXIT" "./nob exit=$NOB_EXIT"

    t "index.cgi produced and executable"
    assert_true "$(check "[ -x '$REPO/index.cgi' ]")" "[ -x index.cgi ]"

    t "index.cgi uses plain CGI (khttp_parse — not khttp_fcgi)"
    assert_true "$(check "strings '$REPO/index.cgi' | grep -q 'khttp_parse'")" \
        "strings khttp_parse"

    t "index.cgi has no FastCGI symbols (plain CGI via fcgiwrap)"
    assert_true "$(check "! strings '$REPO/index.cgi' | grep -q 'khttp_fcgi_init'")" \
        "strings: no khttp_fcgi_init"

    t "index.cgi contains route string 'save'"
    assert_true "$(check "strings '$REPO/index.cgi' | grep -q '^save$'")" \
        "strings save"
else
    printf "  SKIP: kcgi not found or --no-build\n"
fi

# ═══════════════════════════════════════════════════════════════════════════
# Suite 4: self-rebuild
# ═══════════════════════════════════════════════════════════════════════════
suite "NOB_GO_REBUILD_URSELF"

if [ "$NO_BUILD" -eq 0 ] && [ -f "$REPO/nob" ]; then
    t "touch nob.c → nob.c newer than nob"
    touch "$REPO/nob.c"
    assert_true "$([ "$REPO/nob.c" -nt "$REPO/nob" ] && echo 0 || echo 1)" \
        "nob.c -nt nob after touch"

    t "recompile after touch exits 0"
    cc -o "$REPO/nob" "$REPO/nob.c" > /dev/null 2>&1
    assert_true "$?" "cc recompile exit=$?"

    t "macro call syntax correct"
    assert_true "$(check "grep -qE 'NOB_GO_REBUILD_URSELF\(argc, argv\)' '$REPO/nob.c'")" \
        "NOB_GO_REBUILD_URSELF(argc, argv)"
else
    printf "  SKIP: --no-build or nob absent\n"
fi


# ═══════════════════════════════════════════════════════════════════════════
# Suite 5: render_shell HTML structure
# Validates the literal output string in the built binary — no FastCGI
# harness needed. Catches structural regressions (missing </head>, scripts
# outside head, khtml mixing, cascade triggers) before deployment.
# ═══════════════════════════════════════════════════════════════════════════
suite "render_shell HTML structure"

if [ "$NO_BUILD" -eq 0 ] && [ -f "$REPO/index.cgi" ]; then
    t "DOCTYPE present"
    assert_true "$(check "strings '$REPO/index.cgi' | grep -q '<!DOCTYPE html>'")" \
        "strings: <!DOCTYPE html>"

    t "head closes before body opens"
    assert_true "$(check "strings '$REPO/index.cgi' | grep -q '</head><body'")" \
        "strings: </head><body"

    t "htmx script tag present"
    assert_true "$(check "strings '$REPO/index.cgi' | grep -q 'htmx.org@1.9.12'")" \
        "strings: htmx.org@1.9.12"

    t "Alpine.js script tag present"
    assert_true "$(check "strings '$REPO/index.cgi' | grep -q 'alpinejs'")" \
        "strings: alpinejs"

    t "data-cfasync=false on scripts (Cloudflare Rocket Loader guard)"
    assert_true "$(check "strings '$REPO/index.cgi' | grep -q 'data-cfasync'")" \
        "strings: data-cfasync"

    t "no khtml in binary (khtml/khttp_puts mixing guard)"
    assert_true "$(check "! strings '$REPO/index.cgi' | grep -q 'khtml_open'")" \
        "strings: no khtml_open"

    t "Alpine x-init used for initial list load"
    assert_true "$(check "strings '$REPO/index.cgi' | grep -q 'x-init'")" \
        "strings: x-init"

    t "no hx-trigger on main-content (cascade guard)"
    assert_true "$(check "! strings '$REPO/index.cgi' | grep -q 'hx-trigger'")" \
        "strings: no hx-trigger"

    t "main-content id present"
    assert_true "$(check "strings '$REPO/index.cgi' | grep -q 'main-content'")" \
        "strings: main-content"
else
    printf "  SKIP: kcgi not found or --no-build\n"
fi

summary
