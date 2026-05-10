#!/bin/sh
# smoke.sh — end-to-end smoke test using only in-tree tools
#
# Exercises: front end -> skj-as -> skj-ld -> qemu-m68k
# No external cross toolchain needed (no m68k-linux-gnu-*).
#
# Usage: tests/smoke.sh [qemu-binary]
# Requires: build/skj-tinc build/skj-sc build/skj-mooc build/skj-cc build/skj-as build/skj-ld
#
# Made by a machine. PUBLIC DOMAIN (CC0-1.0)

set -eu

QEMU=${1:-qemu-m68k}
HERE=$(dirname "$0")
ROOT=$(cd "$HERE/.." && pwd)
CC="$ROOT/build/skj-cc"
AS="$ROOT/build/skj-as"
LD="$ROOT/build/skj-ld"
BDIR="$ROOT/build/smoke"

pass=0
fail=0

mkdir -p "$BDIR"

# assemble start.S once
"$AS" -o "$BDIR/start.o" "$ROOT/runtime/start.S"

run_test() {
    _name=$1
    _bin="$BDIR/$_name"
    _expect=0

    if [ -f "$HERE/${_name}.exitcode" ]; then
        _expect=$(cat "$HERE/${_name}.exitcode")
    fi

    _input=""
    if [ -f "$HERE/${_name}.input" ]; then
        _input="$HERE/${_name}.input"
    fi

    set +e
    if [ -n "$_input" ]; then
        "$QEMU" "$_bin" <"$_input" >"$BDIR/${_name}.out" 2>&1
    else
        "$QEMU" "$_bin" >"$BDIR/${_name}.out" 2>&1
    fi
    _rc=$?
    set -e

    _ok=1
    if [ "$_rc" -ne "$_expect" ]; then
        _ok=0
    fi
    if [ -f "$HERE/${_name}.expected" ] && \
       ! diff -q "$HERE/${_name}.expected" "$BDIR/${_name}.out" >/dev/null 2>&1; then
        _ok=0
    fi

    if [ "$_ok" -eq 1 ]; then
        printf 'PASS  %s\n' "$_name"
        pass=$((pass + 1))
    else
        printf 'FAIL  %s (rc=%d, want %s)\n' "$_name" "$_rc" "$_expect"
        if [ -f "$HERE/${_name}.expected" ]; then
            diff -u "$HERE/${_name}.expected" "$BDIR/${_name}.out" || true
        fi
        fail=$((fail + 1))
    fi
}

# TinC tests
for f in "$HERE"/*.tc; do
    name=$(basename "$f" .tc)
    "$ROOT/build/skj-tinc" -o "$BDIR/${name}.s" "$f"
    "$AS" -o "$BDIR/${name}.o" "$BDIR/${name}.s"
    "$LD" -o "$BDIR/$name" "$BDIR/start.o" "$BDIR/${name}.o"
    run_test "$name"
done

# Scheme tests
for f in "$HERE"/scm_*.scm; do
    name=$(basename "$f" .scm)
    "$ROOT/build/skj-sc" -o "$BDIR/${name}.s" "$f"
    "$AS" -o "$BDIR/${name}.o" "$BDIR/${name}.s"
    "$LD" -o "$BDIR/$name" "$BDIR/start.o" "$BDIR/${name}.o"
    run_test "$name"
done

# MooScript runtime (cross-compiled with skj-cc, assembled with skj-as)
"$CC" -o "$BDIR/str.s" "$ROOT/runtime/str.c"
"$AS" -o "$BDIR/str.o" "$BDIR/str.s"
"$CC" -o "$BDIR/list.s" "$ROOT/runtime/list.c"
"$AS" -o "$BDIR/list.o" "$BDIR/list.s"
"$CC" -o "$BDIR/host_stub.s" "$ROOT/runtime/host_stub.c"
"$AS" -o "$BDIR/host_stub.o" "$BDIR/host_stub.s"

# MooScript tests
for f in "$HERE"/moo_*.moo; do
    case "$f" in *moo_room.moo|*moo_toy_*) continue ;; esac
    name=$(basename "$f" .moo)
    "$ROOT/build/skj-mooc" -o "$BDIR/${name}.s" "$f"
    "$AS" -o "$BDIR/${name}.o" "$BDIR/${name}.s"
    "$LD" -o "$BDIR/$name" "$BDIR/start.o" "$BDIR/str.o" "$BDIR/list.o" \
       "$BDIR/host_stub.o" "$BDIR/${name}.o"
    run_test "$name"
done

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
