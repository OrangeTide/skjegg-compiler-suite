#!/bin/sh
# run-kp-callback-tests.sh : the Kobold FFI callback boundary suite.  Each Kobold
# program is AOT-compiled, then linked with a C callback stub (cbhost.c) and the
# Kobold runtime, run, and its stdout diffed against the .expected file.  C calls
# Kobold routines passed to it as function pointers, and returns a function pointer
# Kobold calls; the conventions are ABI-identical, so no trampoline is involved.
#
# Driven by the same environment as kobold/aot-run.sh so one script serves every
# target:
#   KPAOT      the single-step AOT driver (.kp -> .s)                    [required]
#   KPAOTLIB   the runtime archive to link                              [required]
#   KPAOTCC    the C compiler that compiles the stub and drives the link [required]
#   KPAOTAS    an explicit assembler for the .s; empty lets KPAOTCC do it
#   KPAOTQEMU  an emulator prefix for a cross target; empty on the host
#   KPAOTNOPIE the link driver's no-PIE flag; default -no-pie
#   KP_CBDIR   the test directory                        [default tests/kp-callback]
set -e

: "${KPAOT:?set KPAOT to the AOT driver}"
: "${KPAOTLIB:?set KPAOTLIB to the runtime archive}"
: "${KPAOTCC:?set KPAOTCC to the C/link driver}"

DIR=${KP_CBDIR:-tests/kp-callback}
NOPIE=${KPAOTNOPIE--no-pie}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# the shared C stub, compiled once with the target's C compiler
$KPAOTCC $NOPIE -c "$DIR/cbhost.c" -o "$tmp/cbhost.o"

pass=0
fail=0
for src in "$DIR"/*.kp; do
    name=$(basename "$src" .kp)
    exp="$DIR/$name.expected"
    [ -f "$exp" ] || { echo "SKIP $name (no .expected)"; continue; }

    "$KPAOT" "$src" -o "$tmp/$name.s"
    if [ -n "$KPAOTAS" ]; then
        $KPAOTAS "$tmp/$name.s" -o "$tmp/$name.o"
    else
        $KPAOTCC $NOPIE -c "$tmp/$name.s" -o "$tmp/$name.o"
    fi
    $KPAOTCC $NOPIE "$tmp/$name.o" "$tmp/cbhost.o" "$KPAOTLIB" -lm -o "$tmp/$name"

    got=$($KPAOTQEMU "$tmp/$name" 2>&1) || true
    want=$(cat "$exp")
    if [ "$got" = "$want" ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "FAIL $name"
        echo "  got: $got" | sed 's/$/ /'
        echo "  want: $want"
    fi
done

echo "kp-callback: $pass passed, $fail failed"
[ $fail -eq 0 ]
