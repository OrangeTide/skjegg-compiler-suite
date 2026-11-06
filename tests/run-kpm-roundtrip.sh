#!/bin/sh
# Check the KPM reader/writer pair is faithful: for each source module, the
# assembly skj-kp emits for the original module must byte-match the assembly it
# emits for the module after a kpm_read -> kpm_write round trip.  Also confirms
# the round trip reaches a fixpoint (a second round trip changes nothing).
#
#   KPC=build/skj-kpc KP=build/skj-kp RT=build/skj-kpm-roundtrip \
#       sh tests/run-kpm-roundtrip.sh file.pas ...
set -e

KPC=${KPC:-build/skj-kpc}
KP=${KP:-build/skj-kp}
RT=${RT:-build/skj-kpm-roundtrip}
TARGET=${TARGET:-KP_TARGET_X64}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fail=0
pass=0
for src in "$@"; do
    base=$(basename "$src" | sed 's/\.[^.]*$//')
    orig="$WORK/$base.kpm"
    rt1="$WORK/$base.rt1.kpm"
    rt2="$WORK/$base.rt2.kpm"
    a="$WORK/$base.a.s"
    b="$WORK/$base.b.s"

    if ! "$KPC" -D "$TARGET" --emit-kpm "$src" > "$orig" 2>"$WORK/$base.emit.err"; then
        echo "FAIL $base: --emit-kpm failed"
        cat "$WORK/$base.emit.err"
        fail=$((fail + 1))
        continue
    fi
    "$RT" "$orig" > "$rt1"
    "$RT" "$rt1" > "$rt2"
    if ! cmp -s "$rt1" "$rt2"; then
        echo "FAIL $base: round trip is not a fixpoint"
        diff "$rt1" "$rt2" | head -20 || true
        fail=$((fail + 1))
        continue
    fi
    "$KP" "$orig" -o "$a"
    "$KP" "$rt1" -o "$b"
    if ! cmp -s "$a" "$b"; then
        echo "FAIL $base: asm differs after round trip"
        diff "$a" "$b" | head -30 || true
        fail=$((fail + 1))
        continue
    fi
    pass=$((pass + 1))
done

echo "kpm-roundtrip: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
