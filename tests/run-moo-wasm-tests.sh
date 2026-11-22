#!/bin/sh
# run-moo-wasm-tests.sh - compile each tests/moo_*.moo with skj-mooc-wasm, link
# it against the MooScript runtime and the synthesized WASI runtime with
# skj-ld-wasm, run it under wasmtime, and check the exit code (the value the
# verb named main returns) against .exitcode/.expect (default 0).
#
# This is the WASM arm of the MooScript corpus, the sibling of the native moo
# lane (check-emu).  The pipeline matches the BASIC/Pascal WASM lanes: the front
# end emits a relocatable object (-o foo.o, no separate assembler), the linker
# synthesizes the crt (_start plus the write/read/exit/sbrk seam over the WASI
# preview1 imports), and wasmtime is the runtime.  A moo program's entry is the
# verb named main returning an int, so the module runs as a plain WASI command
# and its process exit code is that return (no continuations, so no --invoke
# workaround as the scheme lane needs).
#
# Two host runtimes exist, mirroring the native split: host_stub (object ops
# stubbed, the default) and toy_host (a static object DB plus a tell that writes
# stdout, for the moo_toy_* tests).  Both link str.o, list.o and arena_wasm.o
# (the moo bump arena over __libc_sbrk, which the native targets get from their
# crt); toy_host additionally needs va_wasm.o for its varargs tell.  All runtime
# objects are built here with skj-cc-wasm.
#
# A .expected fixture, when present, is diffed against stdout as well.
# moo_room.moo has no main verb (parameterized verbs only), so it is compile-only
# and skipped here, as it is in the native lane.
#
# Environment:
#   MOOCBIN  - skj-mooc-wasm           (default build/skj-mooc-wasm)
#   CCBIN    - skj-cc-wasm             (default build/skj-cc-wasm, builds the runtime)
#   LD       - skj-ld-wasm             (default build/skj-ld-wasm)
#   WASMTIME - the wasm runtime        (default wasmtime)
#   OUTDIR   - where objects/.wasm land (default build/moo-wasm)
#   EXCLUDE  - space-separated test names to skip
#
# A missing wasmtime skips the whole suite cleanly (exit 0), like the other
# WASM lanes.

set -eu

HERE=$(dirname "$0")
ROOT=$(cd "$HERE/.." && pwd)
MOOCBIN="${MOOCBIN:-$ROOT/build/skj-mooc-wasm}"
CCBIN="${CCBIN:-$ROOT/build/skj-cc-wasm}"
LD="${LD:-$ROOT/build/skj-ld-wasm}"
WASMTIME="${WASMTIME:-wasmtime}"
OUTDIR="${OUTDIR:-$ROOT/build/moo-wasm}"
EXCLUDE="${EXCLUDE:-}"
mkdir -p "$OUTDIR"

if [ ! -x "$MOOCBIN" ]; then
    echo "ERROR: $MOOCBIN not found"
    exit 1
fi
if ! command -v "$WASMTIME" >/dev/null 2>&1; then
    echo "run-moo-wasm-tests: wasmtime not found, skipping"
    exit 0
fi

# Shared runtime objects (built with skj-cc-wasm).
str="$OUTDIR/str.o"; list="$OUTDIR/list.o"; arena="$OUTDIR/arena_wasm.o"
stub="$OUTDIR/host_stub.o"; toy="$OUTDIR/toy_host.o"; va="$OUTDIR/va_wasm.o"
"$CCBIN" -c -o "$str"   "$ROOT/runtime/str.c"
"$CCBIN" -c -o "$list"  "$ROOT/runtime/list.c"
"$CCBIN" -c -o "$arena" "$ROOT/runtime/arena_wasm.c"
"$CCBIN" -c -o "$stub"  "$ROOT/runtime/host_stub.c"
"$CCBIN" -c -o "$toy"   "$ROOT/runtime/toy_host.c"
"$CCBIN" -c -o "$va"    "$ROOT/runtime/va_wasm.c"
STUB_RT="$str $list $stub $arena"
TOY_RT="$str $list $toy $arena $va"

fail=0
pass=0
skip=0

for src in "$HERE"/moo_*.moo; do
    [ -f "$src" ] || continue
    name=$(basename "$src" .moo)

    # moo_room has no main verb; it is compile-only, like the native lane.
    if [ "$name" = moo_room ]; then
        printf 'SKIP  %s (compile-only, no main verb)\n' "$name"
        skip=$((skip + 1))
        continue
    fi
    case " $EXCLUDE " in
        *" $name "*)
            printf 'SKIP  %s (excluded)\n' "$name"
            skip=$((skip + 1))
            continue ;;
    esac

    case "$name" in
        moo_toy_*) RT="$TOY_RT" ;;
        *)         RT="$STUB_RT" ;;
    esac

    obj="$OUTDIR/$name.o"
    wasm="$OUTDIR/$name.wasm"
    out="$OUTDIR/$name.out"

    set +e
    "$MOOCBIN" -o "$obj" "$src" 2>/dev/null
    if [ $? -ne 0 ]; then
        printf 'FAIL  %s (compile error)\n' "$name"
        fail=$((fail + 1)); set -e; continue
    fi
    "$LD" -o "$wasm" "$obj" $RT 2>/dev/null
    if [ $? -ne 0 ]; then
        printf 'FAIL  %s (link error)\n' "$name"
        fail=$((fail + 1)); set -e; continue
    fi
    in=/dev/null
    [ -f "$HERE/$name.input" ] && in="$HERE/$name.input"
    "$WASMTIME" run "$wasm" <"$in" >"$out" 2>/dev/null
    rc=$?
    set -e

    expect=0
    [ -f "$HERE/$name.exitcode" ] && expect=$(cat "$HERE/$name.exitcode")
    [ -f "$HERE/$name.expect" ] && expect=$(cat "$HERE/$name.expect")
    if [ "$rc" != "$expect" ]; then
        printf 'FAIL  %s (exit %s, want %s)\n' "$name" "$rc" "$expect"
        fail=$((fail + 1))
        continue
    fi
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
