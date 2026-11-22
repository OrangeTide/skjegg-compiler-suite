#!/bin/sh
# run-pascal-wasm-tests.sh - compile each tests/pascal_t*.pas with skj-pc-wasm,
# link it against the freestanding Pascal runtime with skj-ld-wasm, run it under
# wasmtime, and compare stdout against the .expected fixture and the exit code
# against .exitcode/.expect (stdin fed from a .input fixture when one exists).
#
# This is the WASM arm of the Pascal corpus, the stdout-and-exit sibling of the
# native per-backend Pascal lanes.  skj-pc-wasm emits a relocatable object
# directly; the linker synthesizes the crt (_start plus the write/read/exit
# seams over the WASI preview1 imports); wasmtime is the runtime.  Only one
# runtime object is linked beside the test: pascal_rt.o (the Pascal runtime,
# built freestanding by skj-cc-wasm).  Its managed-string allocation uses the
# runtime's own static bump arena, so no separate arena object is needed.
#
# The real-number tests (_real) and the host-I/O tests (_hostio) are skipped the
# way the ColdFire lane skips them: the freestanding runtime's float formatter is
# a placeholder (it uses no libc), so reals are not exercised here.  EXCLUDE
# carries the remaining tests that need WASM backend or linker features not yet
# built; see the check-pc-wasm comment in the Makefile for the grouping.
#
# Environment:
#   PCBIN    - skj-pc-wasm            (default build/skj-pc-wasm)
#   CCBIN    - skj-cc-wasm            (default build/skj-cc-wasm, builds the runtime)
#   LD       - skj-ld-wasm            (default build/skj-ld-wasm)
#   WASMTIME - the wasm runtime       (default wasmtime)
#   OUTDIR   - where objects/.wasm land (default build/pascal-wasm)
#   EXCLUDE  - space-separated test names to skip
#
# A missing wasmtime skips the whole suite cleanly (exit 0).

set -eu

HERE=$(dirname "$0")
ROOT=$(cd "$HERE/.." && pwd)
PCBIN="${PCBIN:-$ROOT/build/skj-pc-wasm}"
CCBIN="${CCBIN:-$ROOT/build/skj-cc-wasm}"
LD="${LD:-$ROOT/build/skj-ld-wasm}"
WASMTIME="${WASMTIME:-wasmtime}"
OUTDIR="${OUTDIR:-$ROOT/build/pascal-wasm}"
EXCLUDE="${EXCLUDE:-}"
mkdir -p "$OUTDIR"

if [ ! -x "$PCBIN" ]; then
    echo "ERROR: $PCBIN not found"
    exit 1
fi
if ! command -v "$WASMTIME" >/dev/null 2>&1; then
    echo "run-pascal-wasm-tests: wasmtime not found, skipping"
    exit 0
fi

rt="$OUTDIR/pascal_rt.o"
"$CCBIN" -c -o "$rt" "$ROOT/runtime/pascal_rt.c"

fail=0
pass=0
skip=0

for src in "$HERE"/pascal_t*.pas; do
    [ -f "$src" ] || continue
    name=$(basename "$src" .pas)
    case "$name" in
        *_real|*_hostio)
            skip=$((skip + 1))
            continue ;;
    esac
    case " $EXCLUDE " in
        *" $name "*)
            printf 'SKIP  %s (excluded)\n' "$name"
            skip=$((skip + 1))
            continue ;;
    esac

    obj="$OUTDIR/$name.o"
    wasm="$OUTDIR/$name.wasm"
    out="$OUTDIR/$name.out"

    set +e
    "$PCBIN" -o "$obj" "$src" 2>/dev/null
    if [ $? -ne 0 ]; then
        printf 'FAIL  %s (compile error)\n' "$name"
        fail=$((fail + 1)); set -e; continue
    fi
    "$LD" -o "$wasm" "$obj" "$rt" 2>/dev/null
    if [ $? -ne 0 ]; then
        printf 'FAIL  %s (link error)\n' "$name"
        fail=$((fail + 1)); set -e; continue
    fi
    in=/dev/null
    [ -f "$HERE/$name.input" ] && in="$HERE/$name.input"
    "$WASMTIME" run "$wasm" <"$in" >"$out" 2>/dev/null
    rc=$?
    set -e

    expect_rc=0
    [ -f "$HERE/$name.exitcode" ] && expect_rc=$(cat "$HERE/$name.exitcode")
    [ -f "$HERE/$name.expect" ] && expect_rc=$(cat "$HERE/$name.expect")
    if [ "$rc" -ne "$expect_rc" ]; then
        printf 'FAIL  %s (exit %d, want %d)\n' "$name" "$rc" "$expect_rc"
        fail=$((fail + 1))
        continue
    fi
    if [ -f "$HERE/$name.expected" ] && ! diff -u "$HERE/$name.expected" "$out"; then
        printf 'FAIL  %s (stdout mismatch)\n' "$name"
        fail=$((fail + 1))
        continue
    fi
    printf 'PASS  %s (exit %d)\n' "$name" "$rc"
    pass=$((pass + 1))
done

printf '\n%d passed, %d failed, %d skipped\n' "$pass" "$fail" "$skip"
[ "$fail" -eq 0 ]
