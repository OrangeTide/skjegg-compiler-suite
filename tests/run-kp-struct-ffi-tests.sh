#!/bin/sh
# run-kp-struct-ffi-tests.sh : the Kobold struct-FFI boundary suite.  A record
# crosses the C FFI by reference, as a pointer: a `var` record is a C `T*`, a
# `const` record a `const T*`.  Each positive program is AOT-compiled, linked with
# a C glue layer (sfhost.c) that reads and writes the struct through that pointer,
# run, and its stdout diffed against the .expected file.  A program with a
# companion .experr instead expects the COMPILE to fail with that text in its
# diagnostics: those pin the guard that rejects a by-value record argument or a
# record result, which has no implemented C ABI.
#
# Driven by the same environment as kobold/aot-run.sh so one script serves every
# target:
#   KPAOT      the single-step AOT driver (.kp -> .s)                    [required]
#   KPAOTLIB   the runtime archive to link                              [required]
#   KPAOTCC    the C compiler that compiles the glue and drives the link [required]
#   KPAOTAS    an explicit assembler for the .s; empty lets KPAOTCC do it
#   KPAOTQEMU  an emulator prefix for a cross target; empty on the host
#   KPAOTNOPIE the link driver's no-PIE flag; default -no-pie
#   KP_SFDIR   the test directory                     [default tests/kp-struct-ffi]
set -e

: "${KPAOT:?set KPAOT to the AOT driver}"
: "${KPAOTLIB:?set KPAOTLIB to the runtime archive}"
: "${KPAOTCC:?set KPAOTCC to the C/link driver}"

DIR=${KP_SFDIR:-tests/kp-struct-ffi}
NOPIE=${KPAOTNOPIE--no-pie}

# KP_SF_SKIP is a space-separated list of test basenames to skip, for a target
# whose C toolchain cannot run a particular case.  On rv32 the only C compiler is
# zig's riscv32-linux-musl, which is hard-float (ilp32d), while skjegg's rv backend
# uses the soft-float ilp32 convention; the two agree on the integer ABI but not on
# how a float crosses a call, so a test with a float argument or result is skipped
# there.  The integer and by-value struct tests still run.
SKIP=${KP_SF_SKIP:-}
skipped() {
    for _s in $SKIP; do [ "$_s" = "$1" ] && return 0; done
    return 1
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# the shared C glue, compiled once with the target's C compiler
$KPAOTCC $NOPIE -c "$DIR/sfhost.c" -o "$tmp/sfhost.o"

pass=0
fail=0
for src in "$DIR"/*.kp; do
    name=$(basename "$src" .kp)
    if skipped "$name"; then continue; fi
    experr="$DIR/$name.experr"

    if [ -f "$experr" ]; then
        # a compile-must-fail test: skj-kpc must reject it and name the reason
        want=$(cat "$experr")
        if got=$("$KPAOT" "$src" -o "$tmp/$name.s" 2>&1); then
            fail=$((fail + 1))
            echo "FAIL $name (compiled, expected rejection: $want)"
        elif echo "$got" | grep -qF "$want"; then
            pass=$((pass + 1))
        else
            fail=$((fail + 1))
            echo "FAIL $name (wrong diagnostic)"
            echo "  got:  $got"
            echo "  want: *$want*"
        fi
        continue
    fi

    exp="$DIR/$name.expected"
    [ -f "$exp" ] || { echo "SKIP $name (no .expected)"; continue; }

    "$KPAOT" "$src" -o "$tmp/$name.s"
    if [ -n "$KPAOTAS" ]; then
        $KPAOTAS "$tmp/$name.s" -o "$tmp/$name.o"
    else
        $KPAOTCC $NOPIE -c "$tmp/$name.s" -o "$tmp/$name.o"
    fi
    $KPAOTCC $NOPIE "$tmp/$name.o" "$tmp/sfhost.o" "$KPAOTLIB" -lm -o "$tmp/$name"

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

echo "kp-struct-ffi: $pass passed, $fail failed"
[ $fail -eq 0 ]
