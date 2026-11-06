#!/bin/sh
# Single-step equivalence gate.  For each source module:
#   1. the assembly skj-kpc emits in one step (.kp -> .s) must byte-match the
#      assembly the serialized module reaches (skj-kpc --emit-kpm | skj-kp);
#   2. the KPM image skj-kpc --emit-kpm writes must compile (skj-kp) to that same
#      assembly, so the serialized --emit-kpm output is faithful.
# KPC and ONESTEP are the same single-step binary; ONESTEP stays overridable.
#
#   KPC=build/skj-kpc KP=build/skj-kp ONESTEP=build/skj-kpc \
#       sh tests/run-onestep.sh file.pas ...
set -e

KPC=${KPC:-build/skj-kpc}
KP=${KP:-build/skj-kp}
ONESTEP=${ONESTEP:-build/skj-kpc}
TARGET=${TARGET:-KP_TARGET_X64}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fail=0
pass=0
for src in "$@"; do
    base=$(basename "$src" | sed 's/\.[^.]*$//')
    two="$WORK/$base.two.s"
    one="$WORK/$base.one.s"
    viakpm="$WORK/$base.one.kpm"
    onekpm="$WORK/$base.viakpm.s"

    # two-stage reference
    if ! "$KPC" -D "$TARGET" --emit-kpm "$src" > "$WORK/$base.kpm" 2>"$WORK/$base.e1"; then
        echo "FAIL $base: two-stage --emit-kpm"; cat "$WORK/$base.e1"; fail=$((fail+1)); continue
    fi
    "$KP" "$WORK/$base.kpm" -o "$two"

    # single-step, straight to assembly
    if ! "$ONESTEP" -D "$TARGET" "$src" -o "$one" 2>"$WORK/$base.e2"; then
        echo "FAIL $base: single-step compile"; cat "$WORK/$base.e2"; fail=$((fail+1)); continue
    fi
    if ! cmp -s "$two" "$one"; then
        echo "FAIL $base: single-step asm differs from two-stage"
        diff "$two" "$one" | head -30 || true
        fail=$((fail+1)); continue
    fi

    # single-step --emit-kpm, then compile its module: must reach the same asm
    if ! "$ONESTEP" -D "$TARGET" --emit-kpm "$src" -o "$viakpm" 2>"$WORK/$base.e3"; then
        echo "FAIL $base: single-step --emit-kpm"; cat "$WORK/$base.e3"; fail=$((fail+1)); continue
    fi
    "$KP" "$viakpm" -o "$onekpm"
    if ! cmp -s "$two" "$onekpm"; then
        echo "FAIL $base: single-step --emit-kpm module asm differs"
        diff "$two" "$onekpm" | head -30 || true
        fail=$((fail+1)); continue
    fi
    pass=$((pass+1))
done

echo "onestep: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
