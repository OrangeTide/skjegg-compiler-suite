#!/bin/sh
# run-cc-tests.sh - compile, assemble, link, run each tests/cc_*.c under qemu-m68k
# and compare exit code against .exitcode file.

set -eu

HERE=$(dirname "$0")
ROOT=$(cd "$HERE/.." && pwd)
CC="$ROOT/build/skj-cc"
AS="$ROOT/build/skj-as"
LD="m68k-linux-gnu-ld"
QEMU="${QEMU:-qemu-m68k}"
START="$ROOT/build/start.o"

if [ ! -x "$CC" ]; then
    echo "ERROR: $CC not found — run 'make build/skj-cc' first"
    exit 1
fi
if [ ! -x "$AS" ]; then
    echo "ERROR: $AS not found — run 'make build/skj-as' first"
    exit 1
fi
if [ ! -f "$START" ]; then
    "$AS" -o "$START" "$ROOT/runtime/start.S"
fi

fail=0
pass=0

for src in "$HERE"/cc_*.c; do
    [ -f "$src" ] || continue
    name=$(basename "$src" .c)
    exitcode_file="$HERE/$name.exitcode"
    if [ ! -f "$exitcode_file" ]; then
        printf 'SKIP  %s (no .exitcode)\n' "$name"
        continue
    fi

    expect_rc=$(cat "$exitcode_file")
    asm="$ROOT/build/$name.s"
    obj="$ROOT/build/$name.o"
    bin="$ROOT/build/$name"

    set +e
    "$CC" -o "$asm" "$src" 2>/dev/null
    if [ $? -ne 0 ]; then
        printf 'FAIL  %s (compile error)\n' "$name"
        fail=$((fail + 1))
        set -e
        continue
    fi
    "$AS" -o "$obj" "$asm" 2>/dev/null
    if [ $? -ne 0 ]; then
        printf 'FAIL  %s (assemble error)\n' "$name"
        fail=$((fail + 1))
        set -e
        continue
    fi
    "$LD" -o "$bin" "$START" "$obj" 2>/dev/null
    if [ $? -ne 0 ]; then
        printf 'FAIL  %s (link error)\n' "$name"
        fail=$((fail + 1))
        set -e
        continue
    fi
    "$QEMU" "$bin"
    rc=$?
    set -e

    if [ "$rc" -eq "$expect_rc" ]; then
        printf 'PASS  %s (exit %d)\n' "$name" "$rc"
        pass=$((pass + 1))
    else
        printf 'FAIL  %s (exit %d, want %d)\n' "$name" "$rc" "$expect_rc"
        fail=$((fail + 1))
    fi
done

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
