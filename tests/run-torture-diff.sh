#!/bin/sh
# run-torture-diff.sh : cross-backend differential torture (V.E).
#
# For every program in tests/torture/t_*.c, compile and run it on each
# available peer and assert that all peers produce byte-identical stdout and
# the same exit status. A peer whose toolchain or qemu is absent is skipped, and
# the comparison runs among those present (at least two are needed).
#
# The peers are the host gcc, the skj-cc backends under their qemu, the same
# ColdFire and RISC-V binaries under the in-tree emulator skj-run, and the
# in-process x86-64 JIT:
#   gcc      host cc                               (runs natively)
#   cf       skj-cc        -> GNU m68k as/ld     -> qemu-m68k
#   cf-emu   (the cf ELF)                         -> skj-run
#   rv       skj-cc-rv     -> GNU riscv as/ld     -> qemu-riscv32
#   rv-emu   (the rv ELF)                         -> skj-run
#   x86-64   skj-cc-x86-64 -> as / ld             -> qemu-x86_64
#   arm64    skj-cc-arm64  -> GNU aarch64 as/ld   -> qemu-aarch64
#   mips     skj-cc-mips   -> GNU mipsel as/ld    -> qemu-mipsel
#   jit      skj-jit                               (runs in-process)
#
# gcc is the oracle: when present it is the reference every other peer is
# compared against, so a bug uniform across the skj backends (every backend
# agrees with the others, only the real C semantics differ) is caught too.
# Pure cross-backend differential misses that class, and it is exactly where
# this harness's first three finds lived (unsigned ops lowered signed, constant
# typing, implicit i64->int narrowing). It works because the corpus is written
# width- and endianness-independent (t_hex prints the abstract 64-bit integer),
# so gcc's LP64 result matches the skj targets' ILP32 one as long as a program
# names no `long` or pointer-width value, which these do not.
#
# The GNU cross assemblers and linkers are used (not skj-as/skj-ld) so an
# assembler or linker gap cannot be mistaken for a codegen divergence; this
# harness tests the code generators. Float is out of scope: native FPUs diverge
# from the deterministic RV32 core on IEEE-underspecified behavior, which is
# what the separate test-f32-diff suite characterizes. These programs are
# integer, 64-bit, and control-flow only.

set -u

HERE=$(dirname "$0")
ROOT=$(cd "$HERE/.." && pwd)
OUT="${OUTDIR:-$ROOT/build/torture}"
CORPUS="$ROOT/tests/torture"
mkdir -p "$OUT"

# Tool names, overridable like the rest of the Makefile.
GCC=${GCC:-cc}
CC_CF=${CC_CF:-$ROOT/build/skj-cc}
CC_RV=${CC_RV:-$ROOT/build/skj-cc-rv}
CC_X64=${CC_X64:-$ROOT/build/skj-cc-x86-64}
CC_A64=${CC_A64:-$ROOT/build/skj-cc-arm64}
CC_MIPS=${CC_MIPS:-$ROOT/build/skj-cc-mips}
JIT=${JIT:-$ROOT/build/skj-jit}
SKJRUN=${SKJRUN:-$ROOT/build/skj-run}

M68K_AS=${M68K_AS:-m68k-linux-gnu-as}
M68K_LD=${M68K_LD:-m68k-linux-gnu-ld}
RV_AS=${RV_AS:-riscv64-linux-gnu-as}
RV_LD=${RV_LD:-riscv64-linux-gnu-ld}
X64_ASM=${X64_ASM:-as}
X64_ASMFLAGS=${X64_ASMFLAGS:---64}
X64_LD=${X64_LD:-ld}
A64_AS=${A64_AS:-aarch64-linux-gnu-as}
A64_LD=${A64_LD:-aarch64-linux-gnu-ld}
MIPS_AS=${MIPS_AS:-mipsel-none-elf-as}
MIPS_LD=${MIPS_LD:-mipsel-none-elf-ld}

QEMU_M68K=${QEMU:-qemu-m68k}
QEMU_RV=${QEMU_RV:-qemu-riscv32}
QEMU_X64=${QEMU_X64:-qemu-x86_64}
QEMU_A64=${QEMU_A64:-qemu-aarch64}
QEMU_MIPS=${QEMU_MIPS:-qemu-mipsel}

START_CF=${START_CF:-$ROOT/build/start.o}
START_RV=${START_RV:-$ROOT/build/rv/start.o}
START_X64=${START_X64:-$ROOT/build/x86_64/start_sysv.o}
START_A64=${START_A64:-$ROOT/build/arm64/start_aapcs.o}
START_MIPS=${START_MIPS:-$ROOT/build/mips/start.o}

have() { command -v "$1" >/dev/null 2>&1; }
exists() { [ -x "$1" ] || [ -f "$1" ]; }

# Decide which peers are runnable in this environment. cf-emu and rv-emu reuse
# the cf and rv binaries, so they are added only when that AOT peer is present.
PEERS=""
add_peer() { PEERS="$PEERS $1"; }
have_cf=0; have_rv=0
# gcc goes first so it is the reference oracle (the loop uses the first peer as
# the baseline every other peer is compared against).
if have "$GCC"; then add_peer gcc; fi
if exists "$CC_CF" && have "$M68K_AS" && have "$M68K_LD" && have "$QEMU_M68K" \
        && [ -f "$START_CF" ]; then add_peer cf; have_cf=1; fi
if [ "$have_cf" = 1 ] && exists "$SKJRUN"; then add_peer cf-emu; fi
if exists "$CC_RV" && have "$RV_AS" && have "$RV_LD" && have "$QEMU_RV" \
        && [ -f "$START_RV" ]; then add_peer rv; have_rv=1; fi
if [ "$have_rv" = 1 ] && exists "$SKJRUN"; then add_peer rv-emu; fi
if exists "$CC_X64" && have "$X64_ASM" && have "$X64_LD" && have "$QEMU_X64" \
        && [ -f "$START_X64" ]; then add_peer x86-64; fi
if exists "$CC_A64" && have "$A64_AS" && have "$A64_LD" && have "$QEMU_A64" \
        && [ -f "$START_A64" ]; then add_peer arm64; fi
if exists "$CC_MIPS" && have "$MIPS_AS" && have "$MIPS_LD" && have "$QEMU_MIPS" \
        && [ -f "$START_MIPS" ]; then add_peer mips; fi
if exists "$JIT"; then add_peer jit; fi

PEERS=$(echo "$PEERS" | tr ' ' '\n' | sed '/^$/d')

# build_and_run <peer> <src> <stdout-file> ; echoes the exit status, or "x" if
# the peer could not build the program (a compile/assemble/link failure, itself
# a divergence worth reporting).
build_and_run() {
    peer=$1; src=$2; so=$3
    b="$OUT/$(basename "$src" .c).$peer"
    case $peer in
    gcc)
        # the oracle: native host build, libc supplies write/exit.  -w silences
        # the harmless `write` prototype mismatch against libc (torture.h
        # declares it with long/unsigned long, exact on LP64).
        "$GCC" -O2 -w -o "$b.elf" "$src" 2>"$b.err" || { echo x; return; }
        "$b.elf" >"$so" 2>/dev/null; echo $? ;;
    cf)
        "$CC_CF" -o "$b.s" "$src" 2>"$b.err" && \
        "$M68K_AS" -o "$b.o" "$b.s" 2>>"$b.err" && \
        "$M68K_LD" -o "$b.elf" "$START_CF" "$b.o" 2>>"$b.err" || { echo x; return; }
        "$QEMU_M68K" "$b.elf" >"$so" 2>/dev/null; echo $? ;;
    cf-emu)
        "$SKJRUN" "$OUT/$(basename "$src" .c).cf.elf" >"$so" 2>/dev/null; echo $? ;;
    rv)
        "$CC_RV" -o "$b.s" "$src" 2>"$b.err" && \
        "$RV_AS" -march=rv32imfd_zfh -mabi=ilp32 -o "$b.o" "$b.s" 2>>"$b.err" && \
        "$RV_LD" -m elf32lriscv -o "$b.elf" "$START_RV" "$b.o" 2>>"$b.err" || { echo x; return; }
        "$QEMU_RV" "$b.elf" >"$so" 2>/dev/null; echo $? ;;
    rv-emu)
        "$SKJRUN" "$OUT/$(basename "$src" .c).rv.elf" >"$so" 2>/dev/null; echo $? ;;
    x86-64)
        "$CC_X64" -o "$b.s" "$src" 2>"$b.err" && \
        "$X64_ASM" $X64_ASMFLAGS -o "$b.o" "$b.s" 2>>"$b.err" && \
        "$X64_LD" -m elf_x86_64 -o "$b.elf" "$START_X64" "$b.o" 2>>"$b.err" || { echo x; return; }
        "$QEMU_X64" "$b.elf" >"$so" 2>/dev/null; echo $? ;;
    arm64)
        "$CC_A64" -o "$b.s" "$src" 2>"$b.err" && \
        "$A64_AS" -o "$b.o" "$b.s" 2>>"$b.err" && \
        "$A64_LD" -o "$b.elf" "$START_A64" "$b.o" 2>>"$b.err" || { echo x; return; }
        "$QEMU_A64" "$b.elf" >"$so" 2>/dev/null; echo $? ;;
    mips)
        "$CC_MIPS" -o "$b.s" "$src" 2>"$b.err" && \
        "$MIPS_AS" -march=mips1 -EL -o "$b.o" "$b.s" 2>>"$b.err" && \
        "$MIPS_LD" -o "$b.elf" "$START_MIPS" "$b.o" 2>>"$b.err" || { echo x; return; }
        "$QEMU_MIPS" "$b.elf" >"$so" 2>/dev/null; echo $? ;;
    jit)
        "$JIT" "$src" >"$so" 2>/dev/null; echo $? ;;
    esac
}

npeers=$(echo "$PEERS" | sed '/^$/d' | wc -l | tr -d ' ')
echo "peers: $(echo "$PEERS" | tr '\n' ' ')"
if [ "$npeers" -lt 2 ]; then
    echo "need at least two peers; only $npeers available" >&2
    exit 1
fi

ref=$(echo "$PEERS" | head -1)
fails=0
total=0
for src in "$CORPUS"/t_*.c; do
    [ -f "$src" ] || continue
    name=$(basename "$src" .c)
    total=$((total + 1))
    # run every peer, recording "<exit> <stdout-path>"
    bad=""
    refout="$OUT/$name.$ref.out"
    refrc=$(build_and_run "$ref" "$src" "$refout")
    for p in $PEERS; do
        [ "$p" = "$ref" ] && continue
        po="$OUT/$name.$p.out"
        rc=$(build_and_run "$p" "$src" "$po")
        if [ "$rc" != "$refrc" ] || ! cmp -s "$refout" "$po" 2>/dev/null; then
            bad="$bad $p"
        fi
    done
    if [ -n "$bad" ]; then
        printf 'FAIL  %-20s %s diverge from %s (rc %s)\n' \
            "$name" "$(echo "$bad" | sed 's/^ //')" "$ref" "$refrc"
        fails=$((fails + 1))
    else
        printf 'PASS  %-20s %d peers agree (rc %s)\n' "$name" "$npeers" "$refrc"
    fi
done

echo
if [ "$fails" -eq 0 ]; then
    echo "torture-diff: $total programs, $npeers peers, all agree"
    exit 0
fi
echo "torture-diff: $fails of $total programs diverged (see $OUT/<name>.<peer>.out)"
exit 1
