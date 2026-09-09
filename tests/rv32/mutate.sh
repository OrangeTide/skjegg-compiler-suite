#!/bin/sh
# mutate.sh : does the test rig notice when the interpreter is wrong?
# Copyright (c) 2026 Jon Mayo - MIT-0 OR Public Domain
#
# Coverage says which lines ran. It cannot say whether anything would have
# complained had those lines been wrong, and that is the question a test
# suite exists to answer. This introduces one deliberate defect at a time
# into rv32.c, rebuilds, and runs the rig. A mutant that still passes
# everything is a hole: some line was executed, produced a wrong answer,
# and nothing noticed.
#
# Usage: ./mutate.sh [-n count] [-s seed] [-m method] [-v]
#
#   -n  how many mutants to try (default 120)
#   -s  seed for choosing sites (default 1)
#   -m  which rig to run against each mutant, in stages, so that what each
#       stage adds is what the ones before it missed:
#         unit      the suites that need no reference model (default, fast)
#         full      unit, then lockstep and the fuzzer
#         all       full, then the compliance suite
#   -v  name every mutant as it is judged
#
# Survivors are left in the work directory with the diff that produced them.

set -e

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
# The suites this tree added beside the imported seven. They link the
# core alone (no machine.c) and live a directory up.
OWN="test_rv_irq test_rv_expand test_rv_bus test_rv_csr test_rv_decode test_rv_fp"
OWNDIR=${OWNDIR:-"$ROOT/tests"}
EMU=${EMU:-"$ROOT/emu"}
GUEST=${GUEST:-"$ROOT/build/rv32"}
WORK=${WORK:-"$ROOT/build/rv32/mutate-work"}
CORE=${CORE:-"$EMU/rv32.c"}
CC=${CC:-gcc}
CFLAGS="-O1 -g -fno-math-errno -w -I$HERE -I$EMU"
# One ELF reader in the tree; elf_loader.h maps onto it (see icov).
LOADER="$EMU/elf32.c $EMU/guest.c"
COUNT=120
SEED=1
MODE=unit
VERBOSE=0

while [ $# -gt 0 ]; do
    case $1 in
    -n) COUNT=$2; shift 2 ;;
    -s) SEED=$2; shift 2 ;;
    -m) MODE=$2; shift 2 ;;
    -v) VERBOSE=1; shift ;;
    *)  echo "usage: $0 [-n count] [-s seed] [-m unit|full] [-v]" >&2; exit 2 ;;
    esac
done

rm -rf "$WORK"
mkdir -p "$WORK"

# ------------------------------------------------------------ the sites
#
# Every mutation is a single token substitution on one line of rv32.c.
# Comment lines, preprocessor lines and the string literals are left
# alone: changing those produces a mutant that is not a different machine,
# only a different message, and it would be reported as a hole that is not
# one.

# A trailing comment or a string literal is not code, and mutating one
# produces a mutant that is not a different machine, only a different
# message. Blanking them before the scan is what makes that true: the
# earlier version skipped whole-comment lines only, so operators inside
# trailing comments were offered as sites, and two of them were then
# reported as surviving mutants when nothing about the program had
# changed. The scan therefore matches against a blanked copy and records
# the column, and the substitution below uses that column rather than
# searching the line again.
awk '
    function blank(s,   out, i, c, n) {
        out = ""
        n = length(s)
        for (i = 1; i <= n; i++) {
            c = substr(s, i, 1)
            if (c == "/" && substr(s, i + 1, 1) == "*")
                break                   # trailing comment: drop the rest
            if (c == "\"" || c == "'"'"'") {
                q = c
                out = out " "
                for (i++; i <= n; i++) {
                    out = out " "
                    if (substr(s, i, 1) == "\\") { i++; out = out " "; continue }
                    if (substr(s, i, 1) == q) break
                }
                continue
            }
            out = out c
        }
        return out
    }
    function site(pat, from, to,   pos) {
        if (code !~ pat)
            return
        pos = index(code, from)
        if (pos > 0)
            print NR "\t" pos "\t" from "\t" to
    }

    # Skip anything that is not executable code.
    /^[ \t]*[*]/          { next }
    /^[ \t]*\/\*/         { next }
    /^[ \t]*\/\//         { next }
    /^[ \t]*#/            { next }
    /^[ \t]*$/            { next }
    {
        code = blank($0)
        # One record per (line, column, operator) that can be mutated.
        # The patterns are strings rather than regex constants: awk reads
        # a /…/ passed as an argument as a match against the whole record
        # and hands the function a boolean.
        site("[^<>=!+*/-]<[^<=]", "<",   "<=")
        site("[^<>=!]>[^>=]",     ">",   ">=")
        site("<=",                "<=",  "<")
        site(">=",                ">=",  ">")
        site("==",                "==",  "!=")
        site("!=",                "!=",  "==")
        site("&&",                "&&",  "||")
        site("\\|\\|",            "||",  "&&")
        site("[a-z0-9_)] \\+ ",   " + ", " - ")
        site("[a-z0-9_)] - ",     " - ", " + ")
        site("[a-z0-9_)] \\| ",   " | ", " & ")
        site("[a-z0-9_)] & [^&]", " & ", " | ")
        site("<< ",               "<< ", ">> ")
        site(">> ",               ">> ", "<< ")
    }
' "$CORE" > "$WORK/sites.all"

TOTAL_SITES=$(wc -l < "$WORK/sites.all" | tr -d ' ')
echo "$TOTAL_SITES mutable sites in rv32.c"

# Shuffle deterministically from the seed, then take the first COUNT.
awk -v seed="$SEED" 'BEGIN { srand(seed) } { print rand() "\t" $0 }' \
    "$WORK/sites.all" | sort -n | cut -f2- | head -n "$COUNT" \
    > "$WORK/sites"

# ------------------------------------------------------------- the rigs

build_mutant() {
    _src=$1

    $CC $CFLAGS -c "$_src" -o "$WORK/rv32.o" 2>/dev/null || return 1
    for t in test_fp test_mem test_zcmp test_zcb test_bitmanip \
             test_atomic test_irq; do
        $CC $CFLAGS -o "$WORK/$t" "$HERE/$t.c" "$HERE/machine.c" \
            "$WORK/rv32.o" 2>/dev/null || return 1
    done
    $CC $CFLAGS -o "$WORK/harness" "$HERE/test_harness.c" "$HERE/machine.c" \
        $LOADER "$WORK/rv32.o" 2>/dev/null || return 1
    for t in $OWN; do
        $CC $CFLAGS -o "$WORK/$t" "$OWNDIR/$t.c" "$WORK/rv32.o" \
            2>/dev/null || return 1
    done
    if [ "$MODE" = full ] || [ "$MODE" = all ]; then
        $CC $CFLAGS -o "$WORK/lockstep" "$HERE/lockstep.c" "$HERE/machine.c" \
            $LOADER "$HERE/gdbclient.c" "$WORK/rv32.o" \
            2>/dev/null || return 1
        $CC $CFLAGS -o "$WORK/fuzz" "$HERE/fuzz.c" "$HERE/machine.c" \
            $LOADER "$HERE/gdbclient.c" "$WORK/rv32.o" \
            2>/dev/null || return 1
    fi
    if [ "$MODE" = all ]; then
        $CC $CFLAGS -o "$WORK/archtest-run" "$HERE/archtest.c" \
            $LOADER "$WORK/rv32.o" 2>/dev/null || return 1
    fi
    return 0
}

# The compliance suite is minutes per run, so it is asked only about the
# mutants everything cheaper has already failed to notice, and only for the
# suites whose signatures cover the widest part of the machine.
ACT=${ACT:-../../riscv-arch-test/riscv-test-suite}
ARCH_SUITES=${ARCH_SUITES:-I M B}

run_compliance() {
    [ -d "$ACT/env" ] || return 1
    rm -rf "$WORK/archtest-work"
    RUN="$WORK/archtest-run" WORK="$WORK/archtest-work" PORT=31390 \
        timeout 600 "$HERE/run-archtest.sh" "$ACT" $ARCH_SUITES \
        >"$WORK/archtest.log" 2>&1 && return 1
    return 0
}

# Returns 0 when the rig noticed. A timeout counts as noticing: a mutant
# that makes the interpreter loop forever has been detected, just slowly.
run_unit() {
    for t in test_fp test_mem test_zcmp test_zcb test_bitmanip \
             test_atomic test_irq; do
        timeout 20 "$WORK/$t" >/dev/null 2>&1 || return 0
    done
    timeout 20 "$WORK/harness" "$GUEST/test_program.elf" >/dev/null 2>&1 \
        || return 0
    for t in $OWN; do
        timeout 20 "$WORK/$t" >/dev/null 2>&1 || return 0
    done
    return 1
}

# How long the fuzzer runs against each mutant. This is the one knob in
# the rig that is a straight trade of audit time for judgement quality,
# and it was set at 6 rounds, which is nearly decorative: the generator
# is deterministic, so 6 rounds is a fixed 3072 instructions, and the
# defect class the fuzzer is uniquely good at needs hundreds of rounds
# to reach. A divergence needing an operand pair one time in 1600
# survived 120 rounds and died at 600.
#
# 60 is a compromise, not an answer: it costs about 20 seconds per
# mutant that survives the unit stage, and it still cannot see the rare
# operand pairs. A score from this rig is therefore a lower bound on
# what the real suite would catch, since test-rv32-fuzz runs 600.
FUZZ_N=${FUZZ_N:-60}

run_reference() {
    ZB="rv32,zba=true,zbb=true,zbs=true,zcb=true"

    timeout 60 "$WORK/lockstep" "$GUEST/test_program.elf" -p 31380 \
        >/dev/null 2>&1 || return 0
    if [ -f "$GUEST/bitmanip_guest.elf" ]; then
        timeout 60 "$WORK/lockstep" "$GUEST/bitmanip_guest.elf" -cpu "$ZB" \
            -p 31380 >/dev/null 2>&1 || return 0
    fi
    timeout 600 "$WORK/fuzz" "$GUEST/fuzz_target.elf" -n "$FUZZ_N" -q \
        -p 31381 >/dev/null 2>&1 || return 0
    return 1
}

# -------------------------------------------------------------- the run

killed=0
killed_unit=0
killed_ref=0
killed_act=0
survived=0
noncompiling=0
equivalent=0
n=0

: > "$WORK/survivors"

while IFS="	" read -r lineno col from to <&3; do
    n=$((n + 1))
    # Substitute at the column the scan recorded, and literally. sub()
    # would read the pattern as a regex, where " | " is "a space or a
    # space" and matches the first space, and would read "&" in the
    # replacement as the matched text; between them every " | " -> " & "
    # mutant became a whitespace no-op that then survived the whole rig
    # for the excellent reason that it was not a mutant. Searching the
    # line again would also find the operator in a trailing comment
    # rather than the one in the code.
    awk -v ln="$lineno" -v col="$col" -v from="$from" -v to="$to" '
        NR == ln && substr($0, col, length(from)) == from {
            $0 = substr($0, 1, col - 1) to substr($0, col + length(from))
        }
        { print }' \
        "$CORE" > "$WORK/mutant.c"

    if cmp -s "$WORK/mutant.c" "$CORE"; then
        equivalent=$((equivalent + 1))
        continue
    fi
    if ! build_mutant "$WORK/mutant.c"; then
        noncompiling=$((noncompiling + 1))
        continue
    fi

    if run_unit; then
        killed=$((killed + 1))
        killed_unit=$((killed_unit + 1))
        [ "$VERBOSE" = 1 ] && echo "  killed   line $lineno: $from -> $to"
    elif { [ "$MODE" = full ] || [ "$MODE" = all ]; } && run_reference; then
        killed=$((killed + 1))
        killed_ref=$((killed_ref + 1))
        [ "$VERBOSE" = 1 ] &&
            echo "  killed by the reference  line $lineno: $from -> $to"
        echo "reference-only line $lineno: $from -> $to" >> "$WORK/survivors"
    elif [ "$MODE" = all ] && run_compliance; then
        killed=$((killed + 1))
        killed_act=$((killed_act + 1))
        [ "$VERBOSE" = 1 ] &&
            echo "  killed by compliance     line $lineno: $from -> $to"
        echo "compliance-only line $lineno: $from -> $to" >> "$WORK/survivors"
    else
        survived=$((survived + 1))
        echo "SURVIVED line $lineno: $from -> $to" >> "$WORK/survivors"
        [ "$VERBOSE" = 1 ] && echo "  SURVIVED line $lineno: $from -> $to"
        sed -n "${lineno}p" "$CORE" |
            sed "s/^/    was: /" >> "$WORK/survivors"
        cp "$WORK/mutant.c" "$WORK/survivor-$lineno.c"
    fi
done 3< "$WORK/sites"

tried=$((killed + survived))
echo
echo "mutants tried:        $n"
echo "  did not compile:    $noncompiling"
echo "  no textual change:  $equivalent"
echo "  judged:             $tried"
echo "  killed:             $killed"
echo "    by the unit rig:  $killed_unit"
[ "$killed_ref" -gt 0 ] && echo "    only by lockstep or the fuzzer: $killed_ref"
[ "$killed_act" -gt 0 ] && echo "    only by the compliance suite:   $killed_act"
echo "  survived:           $survived"
if [ "$tried" -gt 0 ]; then
    awk -v k="$killed" -v t="$tried" \
        'BEGIN { printf "\nmutation score: %.1f%%\n", k * 100 / t }'
fi
echo
echo "survivors and their lines are in $WORK/survivors"
