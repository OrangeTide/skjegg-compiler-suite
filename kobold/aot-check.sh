#!/bin/sh
# aot-check.sh : run every runnable single-module program in the Kobold emit
# corpus through both the reference JIT runner (kprun) and the ahead-of-time
# toolchain, and report any divergence.  The bar is byte-identical stdout, the
# same one the JIT backends meet.
#
# The module list is read from the Kobold runcheck target so it stays in step
# with it.  A program's own stdin and arguments are supplied where runcheck gives
# them (readtest, runtimeio).  Exit status is compared only as trapped vs clean,
# since the reference and the AOT start file need not share a fault code.
#
# Env selecting the target (as aot-run.sh): KPAOT, KPAOTLIB, KPAOTCC, KPAOTAS,
# KPAOTQEMU.  Also:
#   KPDIR   the Kobold repo (required)
#   KPRUN   the reference runner, e.g. "qemu-riscv32 .../kprun" (required)
#   AOTRUN  path to aot-run.sh (default: beside this script)
here=$(cd "$(dirname "$0")" && pwd)
AOTRUN=${AOTRUN:-$here/aot-run.sh}
: "${KPDIR:?set KPDIR to the Kobold repo}"
: "${KPRUN:?set KPRUN to the reference runner}"
emit="$KPDIR/tests/emit"

mods=$(grep -oE '\$\(KPRUN\) tests/emit/[A-Za-z0-9_]+\.expected' "$KPDIR/Makefile" \
       | sed -E 's#.*/##; s#\.expected##' | sort -u)

pass=0; fail=0; diffs=
for m in $mods; do
    mod="$emit/$m.expected"
    [ -f "$mod" ] || { echo "SKIP $m (missing)"; continue; }

    stdin=/dev/null
    set --
    case $m in
        runtimeio) set -- x y ;;
        readtest)
            stdin=$(mktemp)
            printf '3 4\n5000000000\n2.5\nhello world\nZ\n' > "$stdin" ;;
    esac

    ref=$($KPRUN "$mod" "$@" < "$stdin" 2>/dev/null); rref=$?
    got=$(sh "$AOTRUN" "$mod" "$@" < "$stdin" 2>/dev/null); rgot=$?
    [ "$stdin" = /dev/null ] || rm -f "$stdin"

    # trapped-vs-clean, not the exact code
    tr_ref=0; [ "$rref" -ne 0 ] && tr_ref=1
    tr_got=0; [ "$rgot" -ne 0 ] && tr_got=1

    if [ "$ref" = "$got" ] && [ "$tr_ref" = "$tr_got" ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        diffs="$diffs $m"
        echo "DIFF $m: ref(rc=$rref)=[$ref]  aot(rc=$rgot)=[$got]"
    fi
done

echo "aot-check: $pass passed, $fail differ (${diffs:- none})"
[ "$fail" -eq 0 ]
