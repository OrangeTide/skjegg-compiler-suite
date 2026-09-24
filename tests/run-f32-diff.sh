#!/bin/sh
# run-f32-diff.sh - compile tests/f32_diff.c with a target's skj-cc, link,
# run under that target's qemu, and interpret the exit code against the
# committed golden master.  Exit 0 means every vector matched the RV32
# emulator's f32 core; exit N>0 means vector N-1 was the first to diverge,
# which this script names from build/f32_index.txt.
#
# Selected by environment, exactly like run-cc-tests.sh:
#   CCBIN  - the C compiler         (default build/skj-cc)
#   ASM    - assembler + "-o"       (default "build/skj-as -o")
#   LD     - linker + "-o"          (default "m68k-linux-gnu-ld -o")
#   QEMU   - qemu user-mode         (default qemu-m68k)
#   START  - crt object             (default build/start.o)
#   OUTDIR - scratch dir            (default build)
#   LABEL  - backend name for the report line (default from QEMU)
#   XFAIL  - if non-empty, a divergence is reported but not fatal (for
#            backends known not to be valid f32 sync peers, e.g. ColdFire)

set -eu

HERE=$(dirname "$0")
ROOT=$(cd "$HERE/.." && pwd)
CCBIN="${CCBIN:-$ROOT/build/skj-cc}"
ASM="${ASM:-$ROOT/build/skj-as -o}"
LD="${LD:-m68k-linux-gnu-ld -o}"
QEMU="${QEMU:-qemu-m68k}"
START="${START:-$ROOT/build/start.o}"
OUTDIR="${OUTDIR:-$ROOT/build}"
LABEL="${LABEL:-$QEMU}"
XFAIL="${XFAIL:-}"
IDX="$ROOT/build/f32_index.txt"
mkdir -p "$OUTDIR"

if [ ! -x "$CCBIN" ]; then
    echo "ERROR: $CCBIN not found"; exit 1
fi
if [ ! -f "$ROOT/tests/f32_golden.inc" ]; then
    echo "ERROR: tests/f32_golden.inc missing; run 'make f32-oracle'"; exit 1
fi

asm="$OUTDIR/f32_diff.s"
obj="$OUTDIR/f32_diff.o"
bin="$OUTDIR/f32_diff"

# skj-cc resolves quote-includes from the source's own directory, so
# f32_golden.inc in tests/ is found.
"$CCBIN" -o "$asm" "$ROOT/tests/f32_diff.c"
$ASM "$obj" "$asm"
$LD "$bin" "$START" "$obj"

set +e
"$QEMU" "$bin"
rc=$?
set -e

if [ "$rc" -eq 0 ]; then
    printf 'PASS  %s: all vectors match the f32 oracle\n' "$LABEL"
    exit 0
fi

vec=$((rc - 1))
desc="(no index file)"
[ -f "$IDX" ] && desc=$(awk -F'\t' -v n="$vec" '$1==n {print $2" "$3}' "$IDX")
if [ -n "$XFAIL" ]; then
    printf 'XFAIL %s: diverges at vector %d: %s (known non-sync peer)\n' \
        "$LABEL" "$vec" "$desc"
    exit 0
fi
printf 'FAIL  %s: diverges from the f32 oracle at vector %d: %s\n' \
    "$LABEL" "$vec" "$desc"
exit 1
