#!/bin/sh
# coverage-by-method.sh : what each verification method covers, alone
# Copyright (c) 2026 Jon Mayo - MIT-0 OR Public Domain
#
# An aggregate coverage number says how much of the interpreter the whole
# rig reaches. It does not say which method reached it, and it cannot say
# what any one method contributes that no other does. This runs each method
# on its own against the same instrumented build and records the set of
# lines it executed, which makes both questions answerable.
#
# Usage: ./coverage-by-method.sh [method ...]
#
# Methods: unit program apps archtest fuzz lockstep
# With no arguments it runs the ones whose prerequisites are present.
#
# The expensive ones need a reference model and a checkout:
#   archtest   qemu-system-riscv32 and ACT pointing at riscv-arch-test
#   fuzz       qemu-riscv32
#   lockstep   qemu-riscv32
#   apps       CM and LUA pointing at checkouts

set -e

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
# The suites this tree added beside the imported seven. They link the
# core alone (no machine.c) and live a directory up.
OWN="test_rv_irq test_rv_expand test_rv_bus test_rv_csr test_rv_decode test_rv_fp"
OWNDIR=${OWNDIR:-"$ROOT/tests"}
EMU=${EMU:-"$ROOT/emu"}
GUEST=${GUEST:-"$ROOT/build/rv32"}
WORK=${WORK:-"$ROOT/build/rv32/coverage-work"}
CC=${CC:-gcc}
CFLAGS="-O0 -g --coverage -fno-math-errno -I$HERE -I$EMU"
# One ELF reader in the tree; elf_loader.h maps onto it (see icov).
LOADER="$EMU/elf32.c $EMU/guest.c"
ACT=${ACT:-../../riscv-arch-test/riscv-test-suite}
CM=${CM:-../../coremark}
LUA=${LUA:-../../lua}

ALL="unit program apps archtest fuzz lockstep"
METHODS="$*"

if [ -z "$METHODS" ]; then
    METHODS="unit program"
    if [ -f "$GUEST/coremark.elf" ] || [ -f "$GUEST/lua.elf" ]; then
        METHODS="$METHODS apps"
    fi
    if [ -d "$ACT/env" ]; then
        METHODS="$METHODS archtest"
    fi
    if command -v qemu-riscv32 >/dev/null 2>&1; then
        METHODS="$METHODS fuzz lockstep"
    fi
fi

rm -rf "$WORK"
mkdir -p "$WORK"

# One instrumented object, linked into every tool, so that every method
# writes its counts into the same rv32.gcda and the sets are comparable.
echo "building the instrumented interpreter"
$CC $CFLAGS -c "$EMU/rv32.c" -o "$WORK/rv32.o"

link() {
    _out=$1
    shift
    $CC $CFLAGS -o "$WORK/$_out" "$@" "$WORK/rv32.o" 2>/dev/null
}

link test_fp        "$HERE/test_fp.c" "$HERE/machine.c"
link test_mem       "$HERE/test_mem.c" "$HERE/machine.c"
link test_zcmp      "$HERE/test_zcmp.c" "$HERE/machine.c"
link test_zcb       "$HERE/test_zcb.c" "$HERE/machine.c"
link test_bitmanip  "$HERE/test_bitmanip.c" "$HERE/machine.c"
link test_atomic    "$HERE/test_atomic.c" "$HERE/machine.c"
link test_irq       "$HERE/test_irq.c" "$HERE/machine.c"
for t in $OWN; do
    link "$t" "$OWNDIR/$t.c"
done
link harness        "$HERE/test_harness.c" "$HERE/machine.c" $LOADER
link archtest-run   "$HERE/archtest.c" $LOADER
link fuzz           "$HERE/fuzz.c" "$HERE/machine.c" $LOADER \
                    "$HERE/gdbclient.c"
link lockstep       "$HERE/lockstep.c" "$HERE/machine.c" $LOADER \
                    "$HERE/gdbclient.c"

# The counts accumulate into the object's .gcda, so each method starts from
# a clean one and its result is captured before the next begins.
reset_counts() {
    rm -f "$WORK"/*.gcda
}

capture() {
    _name=$1

    ( cd "$WORK" && gcov -b -o . "$EMU/rv32.c" >/dev/null 2>&1 )
    [ -f "$WORK/rv32.c.gcov" ] || { echo "  no gcov output for $_name" >&2; return; }
    mv "$WORK/rv32.c.gcov" "$WORK/lines-$_name.gcov"

    # A line is covered when its execution count is a number above zero.
    awk -F: '{ c=$1; gsub(/ /,"",c); l=$2; gsub(/ /,"",l);
               if (c ~ /^[0-9]+$/ && c+0 > 0) print l }' \
        "$WORK/lines-$_name.gcov" | sort -u > "$WORK/set-$_name"
    echo "  $_name: $(wc -l < "$WORK/set-$_name") lines"
}

run_unit() {
    for t in test_fp test_mem test_zcmp test_zcb test_bitmanip \
             test_atomic test_irq; do
        "$WORK/$t" >/dev/null 2>&1 || true
    done
}

run_program() {
    "$WORK/harness" "$GUEST/test_program.elf" >/dev/null 2>&1 || true
}

run_apps() {
    for e in coremark.elf lua.elf; do
        if [ -f "$GUEST/$e" ]; then
            "$WORK/harness" "$GUEST/$e" >/dev/null 2>&1 || true
        fi
    done
}

run_archtest() {
    # The real suite runner, told to use the instrumented binary instead of
    # the one built by the Makefile.
    RUN="$WORK/archtest-run" WORK="$WORK/archtest-work" PORT=31360 \
        "$HERE/run-archtest.sh" "$ACT" >/dev/null 2>&1 || true
}

run_fuzz() {
    "$WORK/fuzz" "$GUEST/fuzz_target.elf" -n 20 -q >/dev/null 2>&1 || true
}

run_lockstep() {
    ZB="rv32,zba=true,zbb=true,zbs=true,zcb=true"

    ZCMP="rv32,c=false,zca=true,zcf=true,zcmp=true"

    "$WORK/lockstep" "$GUEST/test_program.elf" >/dev/null 2>&1 || true
    for g in bitmanip_guest.elf zcb_guest.elf; do
        if [ -f "$GUEST/$g" ]; then
            "$WORK/lockstep" "$GUEST/$g" -cpu "$ZB" >/dev/null 2>&1 || true
        fi
    done
    # The whole-frame push and pop guest needs its own reference model, and
    # leaving it out would understate what lockstep reaches.
    if [ -f "$GUEST/test_program_zcmp.elf" ]; then
        "$WORK/lockstep" "$GUEST/test_program_zcmp.elf" -cpu "$ZCMP" \
            >/dev/null 2>&1 || true
    fi
}

echo
echo "running each method alone"
for m in $METHODS; do
    reset_counts
    case $m in
    unit)     run_unit ;;
    program)  run_program ;;
    apps)     run_apps ;;
    archtest) run_archtest ;;
    fuzz)     run_fuzz ;;
    lockstep) run_lockstep ;;
    *)        echo "  unknown method $m" >&2; continue ;;
    esac
    capture "$m"
done

# Everything at once, for the union and the total line count.
reset_counts
for m in $METHODS; do
    case $m in
    unit)     run_unit ;;
    program)  run_program ;;
    apps)     run_apps ;;
    archtest) run_archtest ;;
    fuzz)     run_fuzz ;;
    lockstep) run_lockstep ;;
    esac
done
capture all

TOTAL=$(awk -F: '{ c=$1; gsub(/ /,"",c);
                   if (c ~ /^[0-9]+$/ || c ~ /^#+$/) n++ } END { print n }' \
        "$WORK/lines-all.gcov")

echo
printf '%-10s %8s %8s %9s %9s\n' method lines "of $TOTAL" unique cumulative
printf '%-10s %8s %8s %9s %9s\n' ------ ----- ------- ------ ----------

# unique: covered by this method and by no other.
CUM="$WORK/cum"
: > "$CUM"
for m in $METHODS; do
    others=""
    for o in $METHODS; do
        [ "$o" = "$m" ] || others="$others $WORK/set-$o"
    done
    if [ -n "$others" ]; then
        cat $others | sort -u > "$WORK/others"
    else
        : > "$WORK/others"
    fi
    n=$(wc -l < "$WORK/set-$m")
    u=$(comm -23 "$WORK/set-$m" "$WORK/others" | wc -l)
    cat "$CUM" "$WORK/set-$m" | sort -u > "$WORK/cum.new"
    mv "$WORK/cum.new" "$CUM"
    c=$(wc -l < "$CUM")
    awk -v m="$m" -v n="$n" -v t="$TOTAL" -v u="$u" -v c="$c" \
        'BEGIN { printf "%-10s %8d %7.1f%% %9d %9d\n", m, n, n*100/t, u, c }'
done

U=$(wc -l < "$WORK/set-all")
awk -v n="$U" -v t="$TOTAL" \
    'BEGIN { printf "\nunion      %8d %7.1f%%\n", n, n*100/t }'

echo
echo "lines no method reaches are listed in $WORK/uncovered"
awk -F: '{ c=$1; gsub(/ /,"",c); l=$2; gsub(/ /,"",l); $1=""; $2="";
           if (c ~ /^#+$/) printf "%6s %s\n", l, substr($0,3) }' \
    "$WORK/lines-all.gcov" > "$WORK/uncovered"
wc -l < "$WORK/uncovered" | tr -d ' ' | sed 's/$/ uncovered lines/'
