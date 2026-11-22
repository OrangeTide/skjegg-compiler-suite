#!/bin/sh
# run-basic-wasm-tests.sh - compile each tests/basic_t*.bas with skj-bc-wasm,
# link it against the BASIC runtime and the synthesized WASI runtime with
# skj-ld-wasm, run it under wasmtime, and compare stdout against the .expected
# fixture (stdin fed from a .input fixture when one exists).
#
# This is the WASM arm of the BASIC corpus, the stdout-compared sibling of the
# native per-backend BASIC lanes.  The pipeline is short: skj-bc-wasm emits a
# relocatable object directly (-o foo.o, no separate assembler), the linker
# synthesizes the crt (_start plus the write/read/exit/sbrk seam over the WASI
# preview1 imports), and wasmtime is the runtime.  Beside each test two shared
# runtime objects are linked: basic_rt.o (the BASIC runtime, built with
# skj-cc-wasm) and arena_wasm.o (the moo/basic bump arena over __libc_sbrk,
# which the native targets get from their crt instead).
#
# Environment:
#   BCBIN    - skj-bc-wasm            (default build/skj-bc-wasm)
#   CCBIN    - skj-cc-wasm            (default build/skj-cc-wasm, builds the runtime)
#   LD       - skj-ld-wasm            (default build/skj-ld-wasm)
#   WASMTIME - the wasm runtime       (default wasmtime)
#   OUTDIR   - where objects/.wasm land (default build/basic-wasm)
#   EXCLUDE  - space-separated test names to skip
#
# A missing wasmtime skips the whole suite cleanly (exit 0), matching the other
# WASM lanes.

set -eu

HERE=$(dirname "$0")
ROOT=$(cd "$HERE/.." && pwd)
BCBIN="${BCBIN:-$ROOT/build/skj-bc-wasm}"
CCBIN="${CCBIN:-$ROOT/build/skj-cc-wasm}"
LD="${LD:-$ROOT/build/skj-ld-wasm}"
WASMTIME="${WASMTIME:-wasmtime}"
OUTDIR="${OUTDIR:-$ROOT/build/basic-wasm}"
EXCLUDE="${EXCLUDE:-}"
mkdir -p "$OUTDIR"

if [ ! -x "$BCBIN" ]; then
    echo "ERROR: $BCBIN not found"
    exit 1
fi
if ! command -v "$WASMTIME" >/dev/null 2>&1; then
    echo "run-basic-wasm-tests: wasmtime not found, skipping"
    exit 0
fi

rt="$OUTDIR/basic_rt.o"
arena="$OUTDIR/arena_wasm.o"
"$CCBIN" -DSKJ_LIBC_FLOAT -c -o "$rt" "$ROOT/runtime/basic_rt.c"
"$CCBIN" -c -o "$arena" "$ROOT/runtime/arena_wasm.c"

fail=0
pass=0
skip=0

for src in "$HERE"/basic_t*.bas; do
    [ -f "$src" ] || continue
    name=$(basename "$src" .bas)
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
    "$BCBIN" -o "$obj" "$src" 2>/dev/null
    if [ $? -ne 0 ]; then
        printf 'FAIL  %s (compile error)\n' "$name"
        fail=$((fail + 1)); set -e; continue
    fi
    "$LD" -o "$wasm" "$obj" "$rt" "$arena" 2>/dev/null
    if [ $? -ne 0 ]; then
        printf 'FAIL  %s (link error)\n' "$name"
        fail=$((fail + 1)); set -e; continue
    fi
    in=/dev/null
    [ -f "$HERE/$name.input" ] && in="$HERE/$name.input"
    "$WASMTIME" run "$wasm" <"$in" >"$out" 2>/dev/null
    set -e

    if [ -f "$HERE/$name.expected" ] && ! diff -u "$HERE/$name.expected" "$out"; then
        printf 'FAIL  %s (stdout mismatch)\n' "$name"
        fail=$((fail + 1))
        continue
    fi
    printf 'PASS  %s\n' "$name"
    pass=$((pass + 1))
done

printf '\n%d passed, %d failed, %d skipped\n' "$pass" "$fail" "$skip"
[ "$fail" -eq 0 ]
