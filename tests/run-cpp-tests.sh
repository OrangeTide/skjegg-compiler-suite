#!/bin/sh
# run-cpp-tests.sh - run each tests/cpp_*.c through skj-cpp and compare
# output against the corresponding .expected file.
#
# Usage: run-cpp-tests.sh

set -eu

HERE=$(dirname "$0")
ROOT=$(cd "$HERE/.." && pwd)
CPP="$ROOT/build/skj-cpp"

if [ ! -x "$CPP" ]; then
    echo "ERROR: $CPP not found — run 'make build/skj-cpp' first"
    exit 1
fi

fail=0
pass=0

for src in "$HERE"/cpp_*.c; do
    [ -f "$src" ] || continue
    name=$(basename "$src" .c)
    expected="$HERE/$name.expected"
    if [ ! -f "$expected" ]; then
        printf 'SKIP  %s (no .expected)\n' "$name"
        continue
    fi

    actual="$ROOT/build/$name.out"
    set +e
    "$CPP" "$src" > "$actual" 2>/dev/null
    rc=$?
    set -e

    expect_rc=0
    if [ -f "$HERE/$name.expect" ]; then
        expect_rc=$(cat "$HERE/$name.expect")
    fi

    ok=1
    if [ "$rc" -ne "$expect_rc" ]; then
        ok=0
    fi
    if ! diff -q "$expected" "$actual" >/dev/null 2>&1; then
        ok=0
    fi

    if [ "$ok" -eq 1 ]; then
        printf 'PASS  %s\n' "$name"
        pass=$((pass + 1))
    else
        printf 'FAIL  %s (rc=%d, want %d)\n' "$name" "$rc" "$expect_rc"
        diff -u "$expected" "$actual" || true
        fail=$((fail + 1))
    fi
done

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
