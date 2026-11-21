#!/bin/sh
# run-cc-wasm-tests.sh - compile each tests/cc_*.c with skj-cc-wasm, link it
# against the synthesized WASI runtime with skj-ld-wasm, run it under wasmtime,
# and compare the exit code against the .exitcode file (and stdout against a
# .expected fixture when one exists, stdin fed from a .input fixture).
#
# This is the WASM arm of the C compiler corpus.  The pipeline is shorter than
# run-cc-tests.sh: skj-cc-wasm emits a relocatable object directly (-c, no
# separate assembler), the linker synthesizes the crt (_start plus the
# write/read/exit/sbrk seam over the WASI preview1 imports), and wasmtime is the
# runtime.  Only runtime/va_wasm.c is linked beside the test, for the varargs
# __va_arg helper; the self-contained compute-and-return tests need nothing
# more.
#
# Environment:
#   CCBIN    - skj-cc-wasm            (default build/skj-cc-wasm)
#   LD       - skj-ld-wasm            (default build/skj-ld-wasm)
#   WASMTIME - the wasm runtime       (default wasmtime)
#   OUTDIR   - where objects/.wasm land (default build/cc-wasm)
#   EXCLUDE  - space-separated test names to skip
#
# A missing wasmtime skips the whole suite cleanly (exit 0), matching the other
# WASM lanes.

set -eu

HERE=$(dirname "$0")
ROOT=$(cd "$HERE/.." && pwd)
CCBIN="${CCBIN:-$ROOT/build/skj-cc-wasm}"
LD="${LD:-$ROOT/build/skj-ld-wasm}"
WASMTIME="${WASMTIME:-wasmtime}"
OUTDIR="${OUTDIR:-$ROOT/build/cc-wasm}"
EXCLUDE="${EXCLUDE:-}"
mkdir -p "$OUTDIR"

if [ ! -x "$CCBIN" ]; then
    echo "ERROR: $CCBIN not found"
    exit 1
fi
if ! command -v "$WASMTIME" >/dev/null 2>&1; then
    echo "run-cc-wasm-tests: wasmtime not found, skipping"
    exit 0
fi

va="$OUTDIR/va_wasm.o"
"$CCBIN" -DSKJ_LIBC_FLOAT -c -o "$va" "$ROOT/runtime/va_wasm.c"

fail=0
pass=0
skip=0

for src in "$HERE"/cc_*.c; do
    [ -f "$src" ] || continue
    name=$(basename "$src" .c)
    case " $EXCLUDE " in
        *" $name "*)
            printf 'SKIP  %s (excluded)\n' "$name"
            skip=$((skip + 1))
            continue ;;
    esac
    # Compile-error tests exercise the shared front end, which check-cc already
    # covers; the WASM arm is about codegen and the runtime, so skip them.
    [ -f "$HERE/$name.experror" ] && continue
    exitcode_file="$HERE/$name.exitcode"
    if [ ! -f "$exitcode_file" ]; then
        printf 'SKIP  %s (no .exitcode)\n' "$name"
        skip=$((skip + 1))
        continue
    fi
    expect_rc=$(cat "$exitcode_file")
    obj="$OUTDIR/$name.o"
    wasm="$OUTDIR/$name.wasm"
    out="$OUTDIR/$name.out"

    set +e
    "$CCBIN" -DSKJ_LIBC_FLOAT -c -o "$obj" "$src" 2>/dev/null
    if [ $? -ne 0 ]; then
        printf 'FAIL  %s (compile error)\n' "$name"
        fail=$((fail + 1)); set -e; continue
    fi
    "$LD" -o "$wasm" "$obj" "$va" 2>/dev/null
    if [ $? -ne 0 ]; then
        printf 'FAIL  %s (link error)\n' "$name"
        fail=$((fail + 1)); set -e; continue
    fi
    in=/dev/null
    [ -f "$HERE/$name.input" ] && in="$HERE/$name.input"
    "$WASMTIME" run "$wasm" <"$in" >"$out" 2>/dev/null
    rc=$?
    set -e

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
