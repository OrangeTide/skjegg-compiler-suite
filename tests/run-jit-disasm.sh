#!/bin/sh
# run-jit-disasm.sh - validate a JIT byte sink's output.  JIT each cc test,
# dump its code region (skj-jit-* -d), and confirm the reference disassembler
# decodes every instruction word to a legal instruction.  This catches a
# composed-encoding bug (a wrong movz/movk chain or li/la sequence, a memop
# fallback, or a branch patch) even on a path a runtime test would not reach.
# The pure-code region is disassembled linearly: AArch64 has fixed 4-byte
# instructions, and the RV32 sink emits only 32-bit encodings whose length is
# self-describing (the low two bits are set), so the decode stays aligned; the
# JIT emits no inline literal pools, so there is no data to misread.
#
#   JIT     - the jit driver (default build/skj-jit-arm64)
#   RUNNER  - a command prefix to run it under (e.g. qemu-aarch64)
#   OBJDUMP - the disassembler (default aarch64-linux-gnu-objdump)
#   MARCH   - the objdump machine (default aarch64; e.g. riscv:rv32)
#   EXCLUDE - space-separated test names to skip (default: inline asm only)
#   BIN     - the scratch dump file (default build/jitdis.bin); give each
#             arch its own so a parallel make does not race on one path

set -eu

HERE=$(dirname "$0")
ROOT=$(cd "$HERE/.." && pwd)
JIT="${JIT:-$ROOT/build/skj-jit-arm64}"
RUNNER="${RUNNER:-}"
OBJDUMP="${OBJDUMP:-aarch64-linux-gnu-objdump}"
MARCH="${MARCH:-aarch64}"

# inline asm is not a byte-sink feature (excluded from the JIT suite too); a
# target with more gaps (e.g. RV32 varargs and by-value structs) adds them here
EXCLUDE="${EXCLUDE:-cc_t077_inline_asm}"

bin="${BIN:-$ROOT/build/jitdis.bin}"
fail=0
pass=0
skip=0

for src in "$HERE"/cc_*.c; do
    [ -f "$src" ] || continue
    name=$(basename "$src" .c)
    [ -f "$HERE/$name.experror" ] && continue
    [ -f "$HERE/$name.exitcode" ] || continue
    case " $EXCLUDE " in
        *" $name "*) skip=$((skip + 1)); continue ;;
    esac

    if ! $RUNNER "$JIT" -d "$bin" "$src" >/dev/null 2>&1; then
        printf 'FAIL  %s (jit dump failed)\n' "$name"
        fail=$((fail + 1))
        continue
    fi
    bad=$($OBJDUMP -D -b binary -m "$MARCH" "$bin" 2>/dev/null \
          | grep -ciE 'undefined|\(bad\)' || true)
    if [ "$bad" -eq 0 ]; then
        pass=$((pass + 1))
    else
        printf 'FAIL  %s (%s illegal instruction words)\n' "$name" "$bad"
        $OBJDUMP -D -b binary -m "$MARCH" "$bin" 2>/dev/null \
            | grep -iE 'undefined|\(bad\)' | sed 's/^/    /'
        fail=$((fail + 1))
    fi
done

printf '\n%d ok, %d failed, %d skipped\n' "$pass" "$fail" "$skip"
[ "$fail" -eq 0 ]
