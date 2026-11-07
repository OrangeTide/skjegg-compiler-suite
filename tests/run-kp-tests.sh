#!/bin/sh
# run-kp-tests.sh - exercise the KPM1 module reader (kobold/kpm.c) through the
# single-step Kobold compiler skj-kpc.  This is a self-contained in-tree
# regression for the reader's input validation: it needs no external Kobold
# repo, no qemu, and no assembler or linker, since skj-kpc stops at emitting
# assembly when handed a .kpm.
#
# Each case is a tests/kp/<name>.kpm module.  A case paired with a
# tests/kp/<name>.experror file is a malformed module that must be rejected:
# skj-kpc must fail and its stderr must contain every non-blank, non-'#' line of
# the fixture as a fixed substring (the .experror convention from the Excelsior
# suite).  A case with no .experror is a well-formed module that must compile
# cleanly (exit 0), which guards against the validation falsely rejecting a
# field left at its default.
#
#   KP - the single-step compiler (default build/skj-kpc)

set -eu

HERE=$(dirname "$0")
ROOT=$(cd "$HERE/.." && pwd)
KP=${KP:-"$ROOT/build/skj-kpc"}
BDIR="$ROOT/build/kp"

if [ ! -x "$KP" ]; then
    echo "ERROR: $KP not found"
    exit 1
fi

mkdir -p "$BDIR"

pass=0
fail=0

for src in "$HERE"/kp/*.kpm; do
    name=$(basename "$src" .kpm)
    errfile="$HERE/kp/$name.experror"

    if [ -f "$errfile" ]; then
        # malformed: skj-kpc must fail, and its stderr must carry every line
        set +e
        "$KP" "$src" -o /dev/null >/dev/null 2>"$BDIR/$name.err"
        got=$?
        set -e
        ok=1
        reason=
        if [ "$got" -eq 0 ]; then
            ok=0
            reason="compiled, expected a reader error"
        else
            while IFS= read -r want; do
                case $want in '' | '#'*) continue ;; esac
                if ! grep -qF -- "$want" "$BDIR/$name.err"; then
                    ok=0
                    reason="${reason:+$reason; }missing: $want"
                fi
            done < "$errfile"
        fi
    else
        # well-formed: skj-kpc must compile it cleanly
        set +e
        "$KP" "$src" -o /dev/null >/dev/null 2>"$BDIR/$name.err"
        got=$?
        set -e
        ok=1
        reason=
        if [ "$got" -ne 0 ]; then
            ok=0
            reason="exit $got, expected 0 (should compile)"
        fi
    fi

    if [ "$ok" -eq 1 ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL %s: %s\n' "$name" "$reason"
        sed 's/^/    /' "$BDIR/$name.err"
    fi
done

printf 'kp reader: %d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
