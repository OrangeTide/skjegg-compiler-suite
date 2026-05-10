#!/bin/sh
# run-tests.sh - run each build/<name> under qemu and check the exit
# status against the expected value encoded in tests/<name>.exitcode
# (one integer per file). Tests without an .exitcode file are run for
# their side effects (stdout) and only required to exit 0.
#
# Usage: run-tests.sh [qemu-binary] [build-dir]

set -eu

QEMU=${1:-qemu-m68k}
HERE=$(dirname "$0")
ROOT=$(cd "$HERE/.." && pwd)
BUILDDIR=${2:-build}

fail=0
pass=0

for bin in "$ROOT"/"$BUILDDIR"/*; do
    [ -f "$bin" ] && [ -x "$bin" ] || continue
    name=$(basename "$bin")
    case $name in
        *.o|*.out|start*|skj-*|test_*) continue ;;
    esac
    expect_file="$HERE/$name.exitcode"
    input_file="$HERE/$name.input"
    set +e
    if [ -f "$input_file" ]; then
        "$QEMU" "$bin" <"$input_file" >"$ROOT/$BUILDDIR/$name.out" 2>&1
    else
        "$QEMU" "$bin" >"$ROOT/$BUILDDIR/$name.out" 2>&1
    fi
    rc=$?
    set -e
    if [ -f "$expect_file" ]; then
        want=$(cat "$expect_file")
    else
        want=0
    fi
    expected_file="$HERE/$name.expected"
    ok=1
    if [ "$rc" -ne "$want" ]; then
        ok=0
    fi
    if [ -f "$expected_file" ] && ! diff -q "$expected_file" "$ROOT/$BUILDDIR/$name.out" >/dev/null 2>&1; then
        ok=0
    fi
    if [ "$ok" -eq 1 ]; then
        printf 'PASS  %s\n' "$name"
        pass=$((pass + 1))
    else
        printf 'FAIL  %s (rc=%d, want %s)\n' "$name" "$rc" "$want"
        if [ -f "$expected_file" ]; then
            diff -u "$expected_file" "$ROOT/$BUILDDIR/$name.out" || true
        fi
        fail=$((fail + 1))
    fi
done

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
