#!/bin/sh
# codegen-coverage.sh : per-file line coverage of the code generators.
#
# The rv32 coverage script measures what each verification method contributes
# to one file. This measures the complementary thing for the code generators:
# how much of each shared and per-backend codegen file the whole corpus
# reaches, so the dark regions are visible. The shared selector in particular
# has opcodes only Kobold emits (the 64-bit integer family, the
# overflow-checked ops, the aggregate-return and CALL2 paths), so a C-only run
# leaves large gaps; the Kobold AOT configuration lights those up.
#
# A front end emits assembly as a host program, so every backend's emitter is
# exercised by compiling a corpus to /dev/null on the host. No cross toolchain
# and no qemu are needed: the compiler runs, the target it emits for does not.
#
# Each configuration is built --coverage into its own object directory so the
# gcov notes and counts never collide between configurations built with
# different -D flags. A file built in several configurations (ir/, the shared
# selector, the text sink) has its covered-line sets unioned across them, which
# is the sound way to aggregate counts from incompatible instrumented builds.
#
# Usage: tests/codegen-coverage.sh [config ...]
#   With no arguments it runs every configuration whose tools and corpus are
#   present. Name configurations to run a subset (see CONFIGS below).

set -e

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
WORK=${WORK:-"$ROOT/build/cov"}
CC=${CC:-gcc}
# -Ibuild for the generated version.h every tool's main.c includes.
COV_CFLAGS="-O0 -g --coverage -Wno-unused-parameter -Ibuild"

cd "$ROOT"
make -s build/version.h >/dev/null 2>&1 || true

# The files the report covers, in reading order: the shared IR, each backend's
# regalloc/selector(or emitter)/sink, and the shared text sink.
TARGETS="ir/ir.c ir/util.c ir/arena.c \
         backend/regalloc_cf.c backend/cf_emit.c \
         backend/regalloc_rv.c backend/rv_select.c backend/rv_mc_text.c \
         backend/regalloc_x86.c backend/x86_select.c backend/mc_text.c \
         backend/x86_emit.c backend/x86_aot.c \
         backend/regalloc_arm64.c backend/arm64_select.c backend/arm64_mc_text.c \
         backend/regalloc_mips.c backend/mips_emit.c"

# Each configuration is: a name, the Makefile variable holding its sources, the
# -D/-I flags its tool is built with, and a driver that compiles its corpus
# through the freshly built tool. The sources come from the Makefile so this
# never drifts from how the real tools are built.
CONFIGS="tinc-cf pc-cf moo-cf scheme-cf cc-cf \
         tinc-x64 cc-x64 cc-rv cc-arm64 cc-mips kp-aot"

srcvar() {
    case $1 in
    tinc-cf)   echo SRC ;;
    pc-cf)     echo PC_SRC ;;
    moo-cf)    echo MOO_SRC ;;
    scheme-cf) echo SC_SRC ;;
    cc-cf)     echo CC_SRC ;;
    tinc-x64)  echo SRC_X64 ;;
    cc-x64)    echo CC_SRC_X64 ;;
    cc-rv)     echo CC_SRC_RV ;;
    cc-arm64)  echo CC_SRC_A64 ;;
    cc-mips)   echo CC_SRC_MIPS ;;
    kp-aot)    echo __KP_AOT ;;          # a hand-built source set, below
    esac
}

cflags() {
    case $1 in
    tinc-cf|pc-cf|moo-cf|scheme-cf)
               echo "-Itinc -Imoo -Ipascal -Ischeme -Iir" ;;
    cc-cf)     echo "-DCC_M68K -Icc -Icpp -Iir" ;;
    tinc-x64)  echo "-DX86_BITS=64 -Itinc -Iir -Ibackend" ;;
    cc-x64)    echo "-DX86_BITS=64 -DCC_LP64 -DCC_PSABI -DCC_STRUCT_ABI -Icc -Icpp -Iir -Ibackend" ;;
    cc-rv)     echo "-Icc -Icpp -Iir" ;;
    cc-arm64)  echo "-DCC_LP64 -DCC_PSABI -DCC_ARM64 -Icc -Icpp -Iir -Ibackend" ;;
    cc-mips)   echo "-DCC_PSABI -Icc -Icpp -Iir" ;;
    kp-aot)    echo "-DX86_BITS=64 -Iir -Ibackend -Ikobold" ;;
    esac
}

sources() {
    if [ "$1" = kp-aot ]; then
        echo "kobold/aot_main.c kobold/kpm.c $(make -s print-IR_SRC) $(make -s print-BE_X64_SEL)"
    else
        make -s print-"$(srcvar "$1")"
    fi
}

# Drivers: compile a corpus through $BIN (the just-built instrumented tool),
# discarding the output. Each tolerates per-input failure (a corpus holds
# compile-error fixtures); coverage is what the emitter reached, not the exit.
drive() {
    _name=$1
    _bin=$2
    case $_name in
    tinc-cf|tinc-x64)
        for f in "$ROOT"/tests/*.tc; do
            "$_bin" -o /dev/null "$f" >/dev/null 2>&1 || true
        done ;;
    pc-cf)
        for f in "$ROOT"/tests/pascal_*.pas; do
            "$_bin" -o /dev/null "$f" >/dev/null 2>&1 || true
        done ;;
    moo-cf)
        for f in "$ROOT"/tests/moo_*.moo; do
            "$_bin" -o /dev/null "$f" >/dev/null 2>&1 || true
        done ;;
    scheme-cf)
        for f in "$ROOT"/tests/*.scm; do
            "$_bin" -o /dev/null "$f" >/dev/null 2>&1 || true
        done ;;
    cc-cf|cc-x64|cc-rv|cc-arm64|cc-mips)
        for f in "$ROOT"/tests/cc_t*.c; do
            "$_bin" -o /dev/null "$f" >/dev/null 2>&1 || true
        done ;;
    kp-aot)
        # The AOT compiler reads KPM1 modules; precompile the behavioral
        # corpus with the stock front end, then feed each module through the
        # instrumented skj-kp so the Kobold-only selector paths are exercised.
        for p in "$ROOT"/tests/kp-corpus/*.pas; do
            m="$WORK/kpm/$(basename "$p" .pas).kpm"
            "$_bin" "$m" -o /dev/null >/dev/null 2>&1 || true
        done ;;
    esac
}

# Build the stock skj-kp-jit front end once and precompile the Kobold corpus to
# KPM1 modules the kp-aot configuration consumes.
prepare_kp() {
    mkdir -p "$WORK/kpm"
    make -s build/skj-kp-jit >/dev/null 2>&1 || {
        echo "  (skj-kp-jit did not build; skipping kp-aot)"; return 1; }
    for p in "$ROOT"/tests/kp-corpus/*.pas; do
        "$ROOT/build/skj-kp-jit" --emit-kpm "$p" \
            > "$WORK/kpm/$(basename "$p" .pas).kpm" 2>/dev/null || true
    done
    return 0
}

want="$*"
[ -n "$want" ] || want="$CONFIGS"

rm -rf "$WORK"
mkdir -p "$WORK"

for cfg in $want; do
    dir="$WORK/$cfg"
    mkdir -p "$dir"
    srcs=$(sources "$cfg")
    cf=$(cflags "$cfg")

    if [ "$cfg" = kp-aot ]; then
        prepare_kp || continue
    fi

    # Compile each source to an object named by its path so basenames never
    # collide, then link. The .gcno land beside the objects in $dir.
    # Objects are named by source basename (unique within a configuration) so
    # gcov -o "$dir" "$src" finds the matching <basename>.gcno / .gcda.
    objs=""
    ok=1
    for s in $srcs; do
        o="$dir/$(basename "$s" .c).o"
        if ! $CC $COV_CFLAGS $cf -c "$s" -o "$o" 2>"$dir/build.log"; then
            echo "  $cfg: build failed ($s); see $dir/build.log" >&2
            ok=0
            break
        fi
        objs="$objs $o"
    done
    [ "$ok" = 1 ] || continue
    if ! $CC $COV_CFLAGS -o "$dir/tool" $objs 2>>"$dir/build.log"; then
        echo "  $cfg: link failed; see $dir/build.log" >&2
        continue
    fi

    echo "running $cfg"
    drive "$cfg" "$dir/tool"

    # gcov every target file this configuration built, keeping each covered and
    # instrumentable line set for the union below.
    # gcov resolves each source at the (relative) path recorded in the .gcno,
    # so it runs from $ROOT; the .gcov lands here and is removed after reading.
    for t in $TARGETS; do
        [ -f "$dir/$(basename "$t" .c).gcda" ] || continue
        gcov -o "$dir" "$t" >/dev/null 2>&1 || continue
        g="$ROOT/$(basename "$t").gcov"
        [ -f "$g" ] || continue
        key=$(echo "$t" | tr / _)
        # covered line numbers (count > 0)
        awk -F: '{ c=$1; gsub(/ /,"",c); l=$2; gsub(/ /,"",l);
                   if (c ~ /^[0-9]+$/ && c+0 > 0) print l }' "$g" \
            >> "$WORK/cov-$key"
        # instrumentable line numbers (a count or ##### marker)
        awk -F: '{ c=$1; gsub(/ /,"",c); l=$2; gsub(/ /,"",l);
                   if (c ~ /^[0-9]+$/ || c ~ /^#+$/) print l }' "$g" \
            >> "$WORK/inst-$key"
        rm -f "$g"
    done
done

echo
printf '%-28s %7s %7s %6s\n' file covered of pct
printf '%-28s %7s %7s %6s\n' ---- ------- ------- ----
grand_c=0
grand_t=0
for t in $TARGETS; do
    key=$(echo "$t" | tr / _)
    [ -f "$WORK/inst-$key" ] || continue
    # Line numbers sorted lexically and uniquely, the order comm needs.
    sort -u "$WORK/inst-$key" > "$WORK/inst-$key.s"
    sort -u "$WORK/cov-$key" 2>/dev/null > "$WORK/cov-$key.s" || : > "$WORK/cov-$key.s"
    covered=$(wc -l < "$WORK/cov-$key.s" | tr -d ' ')
    total=$(wc -l < "$WORK/inst-$key.s" | tr -d ' ')
    [ "$total" -gt 0 ] || continue
    grand_c=$((grand_c + covered))
    grand_t=$((grand_t + total))
    # uncovered = instrumentable minus covered, numerically sorted for reading
    comm -23 "$WORK/inst-$key.s" "$WORK/cov-$key.s" | sort -n > "$WORK/uncov-$key"
    awk -v f="$t" -v c="$covered" -v n="$total" \
        'BEGIN { printf "%-28s %7d %7d %5.1f%%\n", f, c, n, c*100/n }'
done
printf '%-28s %7s %7s %6s\n' ---- ------- ------- ----
awk -v c="$grand_c" -v n="$grand_t" \
    'BEGIN { printf "%-28s %7d %7d %5.1f%%\n", "total", c, n, (n?c*100/n:0) }'

echo
echo "per-file uncovered line numbers are in $WORK/uncov-<file>"
