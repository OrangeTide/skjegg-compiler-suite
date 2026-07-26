#!/bin/sh
# run-exc-tests.sh — Excelsior end-to-end tests using only in-tree tools
#
# Exercises: skj-exc -> skj-as -> skj-ld -> qemu-m68k
#
# Each tests/exs_*.exs with a matching .exitcode and/or .expected file is
# compiled, linked against runtime/start.S and the test host
# (runtime/exc_host.c, which bootstraps the entry actor, provides __exc_send,
# and the log/string helpers), and run under qemu. Its exit code is compared
# to the .exitcode file (default 0), and, when a .expected file is present,
# its stdout is diffed against it. Files with neither are skipped (reader /
# feature samples).
#
# A .experror file instead marks a test that must NOT compile: skj-exc is
# expected to fail, and every line of the .experror file must appear
# somewhere in its stderr (as a fixed substring, so a message can be pinned
# without pinning the line number or the file path around it). Blank lines
# and lines starting with # are comments. This is how a teaching error is
# held in place: the message is the feature, so a silent rewording is a
# test failure. A .experror test has no .exs run and no .expected.
#
# Usage: tests/run-exc-tests.sh [qemu-binary]
# Requires: build/skj-exc build/skj-as build/skj-ld build/start.o
#           build/exc_host.o
#
# Made by a machine. PUBLIC DOMAIN (CC0-1.0)

set -eu

QEMU=${1:-qemu-m68k}
HERE=$(dirname "$0")
ROOT=$(cd "$HERE/.." && pwd)
EXC="$ROOT/build/skj-exc"
AS="$ROOT/build/skj-as"
LD="$ROOT/build/skj-ld"
BDIR="$ROOT/build/exc"
START="$ROOT/build/start.o"
HOST="$ROOT/build/exc_host.o"
UTF8="$ROOT/build/utf8.o"

pass=0
fail=0

mkdir -p "$BDIR"

# test names are those with a .exitcode, .expected, or .experror file
names=$(ls "$HERE"/exs_*.exitcode "$HERE"/exs_*.expected \
           "$HERE"/exs_*.experror 2>/dev/null |
        sed -e 's#.*/##' -e 's/\.exitcode$//' -e 's/\.expected$//' \
            -e 's/\.experror$//' |
        sort -u)

for name in $names; do
    src="$HERE/$name.exs"
    ecfile="$HERE/$name.exitcode"
    expfile="$HERE/$name.expected"
    errfile="$HERE/$name.experror"

    # a compile-error test: skj-exc must fail, and its stderr must carry
    # every line of the fixture
    if [ -f "$errfile" ]; then
        set +e
        "$EXC" -o /dev/null "$src" >/dev/null 2>"$BDIR/$name.err"
        got=$?
        set -e
        ok=1
        reason=
        if [ "$got" -eq 0 ]; then
            ok=0
            reason="compiled, expected a compile error"
        else
            while IFS= read -r want; do
                case $want in '' | '#'*) continue ;; esac
                if ! grep -qF -- "$want" "$BDIR/$name.err"; then
                    ok=0
                    reason="${reason:+$reason; }missing: $want"
                fi
            done < "$errfile"
        fi
        if [ "$ok" -eq 1 ]; then
            pass=$((pass + 1))
        else
            fail=$((fail + 1))
            printf 'FAIL %s: %s\n' "$name" "$reason"
            sed 's/^/    /' "$BDIR/$name.err"
        fi
        continue
    fi

    expect=0
    [ -f "$ecfile" ] && expect=$(cat "$ecfile")

    if "$EXC" -o "$BDIR/$name.s" "$src" 2>"$BDIR/$name.err" &&
       "$AS" -o "$BDIR/$name.o" "$BDIR/$name.s" 2>>"$BDIR/$name.err" &&
       "$LD" -o "$BDIR/$name" "$START" "$HOST" "$UTF8" "$BDIR/$name.o" \
             2>>"$BDIR/$name.err"
    then
        set +e
        "$QEMU" "$BDIR/$name" >"$BDIR/$name.out" 2>/dev/null
        got=$?
        set -e
        ok=1
        reason=
        if [ "$got" -ne "$expect" ]; then
            ok=0
            reason="exit $got, expected $expect"
        fi
        if [ -f "$expfile" ] && ! diff -u "$expfile" "$BDIR/$name.out" \
                                     >"$BDIR/$name.diff" 2>&1
        then
            ok=0
            reason="${reason:+$reason; }stdout differs"
        fi
        if [ "$ok" -eq 1 ]; then
            pass=$((pass + 1))
        else
            fail=$((fail + 1))
            printf 'FAIL %s: %s\n' "$name" "$reason"
            [ -f "$expfile" ] && [ -s "$BDIR/$name.diff" ] &&
                sed 's/^/    /' "$BDIR/$name.diff"
        fi
    else
        fail=$((fail + 1))
        printf 'FAIL %s: compile/link error\n' "$name"
        sed 's/^/    /' "$BDIR/$name.err"
    fi
done

printf 'exc tests: %d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
