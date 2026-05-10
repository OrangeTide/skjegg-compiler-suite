#!/bin/sh
# check-as.sh - build and run all tests using skj-as instead of m68k-linux-gnu-as
#
# Usage: tests/check-as.sh [qemu-binary]
#
# Assembles runtime/start.S and each compiler's .s output with skj-as,
# links with m68k-linux-gnu-ld, runs under qemu, and checks results.
# Requires: build/skj-{tinc,sc,mooc,pc,as} already built (run make first).

set -eu

QEMU=${1:-qemu-m68k}
HERE=$(dirname "$0")
ROOT=$(cd "$HERE/.." && pwd)
AS="$ROOT/build/skj-as"
LD=m68k-linux-gnu-ld
M68K_CC=m68k-linux-gnu-gcc
BDIR="$ROOT/build/as-check"

mkdir -p "$BDIR"

# assemble start.S
"$AS" -o "$BDIR/start.o" "$ROOT/runtime/start.S"

# cross-compile runtime objects (these need gcc, not our assembler)
$M68K_CC -std=c99 -O2 -Wall -ffreestanding -c -o "$BDIR/pascal_rt.o" "$ROOT/runtime/pascal_rt.c"
$M68K_CC -std=c99 -O2 -Wall -ffreestanding -c -o "$BDIR/str.o" "$ROOT/runtime/str.c"
$M68K_CC -std=c99 -O2 -Wall -ffreestanding -c -o "$BDIR/list.o" "$ROOT/runtime/list.c"
$M68K_CC -std=c99 -O2 -Wall -ffreestanding -c -o "$BDIR/host_stub.o" "$ROOT/runtime/host_stub.c"
$M68K_CC -std=c99 -O2 -Wall -ffreestanding -c -o "$BDIR/toy_host.o" "$ROOT/runtime/toy_host.c"

pass=0
fail=0

run_test() {
    name=$1
    bin="$BDIR/$name"

    expect=0
    if [ -f "$HERE/${name}.exitcode" ]; then
        expect=$(cat "$HERE/${name}.exitcode")
    fi

    input=""
    if [ -f "$HERE/${name}.input" ]; then
        input="$HERE/${name}.input"
    fi

    set +e
    if [ -n "$input" ]; then
        "$QEMU" "$bin" <"$input" >"$BDIR/${name}.out" 2>&1
    else
        "$QEMU" "$bin" >"$BDIR/${name}.out" 2>&1
    fi
    rc=$?
    set -e

    ok=1
    if [ "$rc" -ne "$expect" ]; then
        ok=0
    fi
    if [ -f "$HERE/${name}.expected" ] && \
       ! diff -q "$HERE/${name}.expected" "$BDIR/${name}.out" >/dev/null 2>&1; then
        ok=0
    fi

    if [ "$ok" -eq 1 ]; then
        pass=$((pass + 1))
    else
        printf 'FAIL  %s (rc=%d, want %s)\n' "$name" "$rc" "$expect"
        if [ -f "$HERE/${name}.expected" ]; then
            diff -u "$HERE/${name}.expected" "$BDIR/${name}.out" || true
        fi
        fail=$((fail + 1))
    fi
}

# TinC tests
for f in "$HERE"/*.tc; do
    name=$(basename "$f" .tc)
    "$ROOT/build/skj-tinc" -o "$BDIR/${name}.s" "$f"
    "$AS" -o "$BDIR/${name}.o" "$BDIR/${name}.s"
    $LD -o "$BDIR/$name" "$BDIR/start.o" "$BDIR/${name}.o" 2>/dev/null
    run_test "$name"
done

# Scheme tests
for f in "$HERE"/scm_*.scm; do
    name=$(basename "$f" .scm)
    "$ROOT/build/skj-sc" -o "$BDIR/${name}.s" "$f"
    "$AS" -o "$BDIR/${name}.o" "$BDIR/${name}.s"
    $LD -o "$BDIR/$name" "$BDIR/start.o" "$BDIR/${name}.o" 2>/dev/null
    run_test "$name"
done

# MooScript tests (host_stub runtime)
for f in "$HERE"/moo_*.moo; do
    case "$f" in *moo_room.moo|*moo_toy_*.moo) continue ;; esac
    name=$(basename "$f" .moo)
    "$ROOT/build/skj-mooc" -o "$BDIR/${name}.s" "$f"
    "$AS" -o "$BDIR/${name}.o" "$BDIR/${name}.s"
    $LD -o "$BDIR/$name" "$BDIR/start.o" "$BDIR/str.o" "$BDIR/list.o" \
       "$BDIR/host_stub.o" "$BDIR/${name}.o" 2>/dev/null
    run_test "$name"
done

# MooScript toy tests (toy_host runtime)
for f in "$HERE"/moo_toy_*.moo; do
    name=$(basename "$f" .moo)
    "$ROOT/build/skj-mooc" -o "$BDIR/${name}.s" "$f"
    "$AS" -o "$BDIR/${name}.o" "$BDIR/${name}.s"
    $LD -o "$BDIR/$name" "$BDIR/start.o" "$BDIR/str.o" "$BDIR/list.o" \
       "$BDIR/toy_host.o" "$BDIR/${name}.o" 2>/dev/null
    run_test "$name"
done

# Pascal tests
for f in "$HERE"/pascal_*.pas; do
    name=$(basename "$f" .pas)
    "$ROOT/build/skj-pc" -o "$BDIR/${name}.s" "$f"
    "$AS" -o "$BDIR/${name}.o" "$BDIR/${name}.s"
    $LD -o "$BDIR/$name" "$BDIR/start.o" "$BDIR/pascal_rt.o" \
       "$BDIR/${name}.o" 2>/dev/null
    run_test "$name"
done

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
