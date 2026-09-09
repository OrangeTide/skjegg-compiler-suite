#!/bin/sh
# RV32 assembler + linker test runner.
#
# Encoding tests (rvas/enc_*.s): assemble with skj-as-rv and with the GNU
#   assembler, then compare the .text and .data section bytes.  The GNU
#   assembler is the golden master, so a mismatch is a skj-as-rv encoding bug.
#
# Run tests (rvas/run_*.s): assemble with skj-as-rv, link with skj-ld-rv, run
#   the result on both qemu-riscv32 and the in-tree emulator (skj-run), and
#   check the exit code (0, or the value in a matching .exitcode file).  The
#   same source is also built with the GNU toolchain and run under qemu as a
#   cross-check that the program itself is correct.
#
# Tools are overridable by environment variable.

DIR=$(dirname "$0")/rvas
SKJ=${SKJ:-./build/skj-as-rv}
SKJLD=${SKJLD:-./build/skj-ld-rv}
RUN=${RUN:-./build/skj-run}
GAS=${GAS:-riscv64-linux-gnu-as}
GLD=${GLD:-riscv64-linux-gnu-ld}
OBJCOPY=${OBJCOPY:-riscv64-linux-gnu-objcopy}
QEMU=${QEMU:-qemu-riscv32}
GAS_ARCH="-march=rv32imafd_zfh_zba_zbb_zbs -mabi=ilp32 -mno-relax"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
pass=0
fail=0

# extract one section's bytes to a file (empty file if the section is absent)
extract() {
    "$OBJCOPY" -O binary --only-section="$2" "$1" "$3" 2>/dev/null || :
    [ -f "$3" ] || : > "$3"
}

echo "== encoding tests (golden master: $GAS) =="
for s in "$DIR"/enc_*.s; do
    [ -f "$s" ] || continue
    name=$(basename "$s" .s)
    if ! "$SKJ" -o "$TMP/$name.skj.o" "$s" 2>"$TMP/err"; then
        echo "FAIL $name (skj-as-rv error)"; sed 's/^/    /' "$TMP/err"
        fail=$((fail + 1)); continue
    fi
    if ! "$GAS" $GAS_ARCH -o "$TMP/$name.gas.o" "$s" 2>"$TMP/err"; then
        echo "SKIP $name (GNU as could not assemble it)"; continue
    fi
    ok=1
    for sec in .text .data; do
        extract "$TMP/$name.skj.o" "$sec" "$TMP/$name.skj$sec"
        extract "$TMP/$name.gas.o" "$sec" "$TMP/$name.gas$sec"
        cmp -s "$TMP/$name.skj$sec" "$TMP/$name.gas$sec" || ok=0
    done
    if [ "$ok" = 1 ]; then
        echo "PASS $name"; pass=$((pass + 1))
    else
        echo "FAIL $name (encoding mismatch vs GNU as)"; fail=$((fail + 1))
    fi
done

echo "== run tests (skj-ld-rv + skj-run + $QEMU) =="
for s in "$DIR"/run_*.s; do
    [ -f "$s" ] || continue
    name=$(basename "$s" .s)
    exp=0
    [ -f "$DIR/$name.exitcode" ] && exp=$(cat "$DIR/$name.exitcode")

    if ! "$SKJ" -o "$TMP/$name.o" "$s" 2>"$TMP/err"; then
        echo "FAIL $name (skj-as-rv error)"; sed 's/^/    /' "$TMP/err"
        fail=$((fail + 1)); continue
    fi
    if ! "$SKJLD" -o "$TMP/$name.elf" "$TMP/$name.o" 2>"$TMP/err"; then
        echo "FAIL $name (skj-ld-rv error)"; sed 's/^/    /' "$TMP/err"
        fail=$((fail + 1)); continue
    fi

    "$RUN" "$TMP/$name.elf"; run_rc=$?
    "$QEMU" "$TMP/$name.elf"; qemu_rc=$?

    # cross-check: same source through the GNU toolchain, run under qemu
    gnu_rc="skip"
    if "$GAS" $GAS_ARCH -o "$TMP/$name.gas.o" "$s" 2>/dev/null &&
       "$GLD" -m elf32lriscv -o "$TMP/$name.gas.elf" "$TMP/$name.gas.o" 2>/dev/null; then
        "$QEMU" "$TMP/$name.gas.elf"; gnu_rc=$?
    fi

    if [ "$run_rc" = "$exp" ] && [ "$qemu_rc" = "$exp" ] &&
       { [ "$gnu_rc" = "skip" ] || [ "$gnu_rc" = "$exp" ]; }; then
        echo "PASS $name (exit $exp; skj-run+qemu+gnu agree)"; pass=$((pass + 1))
    else
        echo "FAIL $name (want $exp; skj-run=$run_rc qemu=$qemu_rc gnu=$gnu_rc)"
        fail=$((fail + 1))
    fi
done

echo ""
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
