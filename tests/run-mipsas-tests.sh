#!/bin/sh
# MIPS I assembler test runner (skj-as-mips).
#
# Encoding tests (mipsas/enc_*.s): assemble with skj-as-mips and with GNU as,
#   then compare the .text and .data section bytes.  GNU as is the golden
#   master, so a mismatch is a skj-as-mips encoding or macro-expansion bug.
#   These are .set noreorder, where skj-as-mips matches GNU byte for byte.
#
# Run tests (mipsas/run_*.s): assemble with skj-as-mips, link with skj-ld-mips,
#   run under qemu-mipsel and check the exit code (0, or
#   the value in a matching .exitcode file).  These are .set reorder, so they
#   exercise the delay-slot scheduler end to end.
#
# spim tests (mipsas/spim_*.s): .set reorder programs in the SPIM syscall ABI.
#   skj-as-mips -S emits the scheduled assembly, which is run under
#   `spim -delayed_loads -delayed_branches`, the accurate R2000 model that
#   qemu's interlocked pipeline cannot show.  A missing load-delay nop reads
#   stale data and fails the exit-code check.  Skipped if spim is absent.
#
# Tools are overridable by environment variable.

DIR=$(dirname "$0")/mipsas
SKJ=${SKJ:-./build/skj-as-mips}
GAS=${GAS:-mipsel-none-elf-as}
GLD=${GLD:-mipsel-none-elf-ld}
SKJLD=${SKJLD:-./build/skj-ld-mips}
OBJCOPY=${OBJCOPY:-mipsel-none-elf-objcopy}
QEMU=${QEMU:-qemu-mipsel}
SPIM=${SPIM:-spim}
GAS_ARCH="-march=mips1 -EL"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
pass=0
fail=0

extract() {
    "$OBJCOPY" -O binary --only-section="$2" "$1" "$3" 2>/dev/null || :
    [ -f "$3" ] || : > "$3"
}

echo "== encoding tests (golden master: $GAS) =="
for s in "$DIR"/enc_*.s; do
    [ -f "$s" ] || continue
    name=$(basename "$s" .s)
    if ! "$SKJ" -o "$TMP/$name.skj.o" "$s" 2>"$TMP/err"; then
        echo "FAIL $name (skj-as-mips error)"; sed 's/^/    /' "$TMP/err"
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

echo ""
echo "== run tests ($SKJLD + $QEMU) =="
for s in "$DIR"/run_*.s; do
    [ -f "$s" ] || continue
    name=$(basename "$s" .s)
    exp=0
    [ -f "$DIR/$name.exitcode" ] && exp=$(cat "$DIR/$name.exitcode")

    if ! "$SKJ" -o "$TMP/$name.o" "$s" 2>"$TMP/err"; then
        echo "FAIL $name (skj-as-mips error)"; sed 's/^/    /' "$TMP/err"
        fail=$((fail + 1)); continue
    fi
    if ! "$SKJLD" -e __start -o "$TMP/$name.elf" "$TMP/$name.o" 2>"$TMP/err"; then
        echo "FAIL $name (link error)"; sed 's/^/    /' "$TMP/err"
        fail=$((fail + 1)); continue
    fi
    "$QEMU" "$TMP/$name.elf"; rc=$?
    if [ "$rc" = "$exp" ]; then
        echo "PASS $name (exit $exp)"; pass=$((pass + 1))
    else
        echo "FAIL $name (want $exp, got $rc)"; fail=$((fail + 1))
    fi
done

echo ""
if command -v "$SPIM" >/dev/null 2>&1; then
    echo "== spim R2000 load-delay tests ($SPIM -delayed_loads) =="
    for s in "$DIR"/spim_*.s; do
        [ -f "$s" ] || continue
        name=$(basename "$s" .s)
        exp=0
        [ -f "$DIR/$name.exitcode" ] && exp=$(cat "$DIR/$name.exitcode")
        if ! "$SKJ" -S -o "$TMP/$name.sched.s" "$s" 2>"$TMP/err"; then
            echo "FAIL $name (skj-as-mips -S error)"; sed 's/^/    /' "$TMP/err"
            fail=$((fail + 1)); continue
        fi
        "$SPIM" -delayed_loads -delayed_branches -f "$TMP/$name.sched.s" \
            >/dev/null 2>&1; rc=$?
        if [ "$rc" = "$exp" ]; then
            echo "PASS $name (spim exit $exp)"; pass=$((pass + 1))
        else
            echo "FAIL $name (want $exp, got $rc; a delay-slot hazard)"
            fail=$((fail + 1))
        fi
    done
else
    echo "== spim tests skipped (spim not found) =="
fi

echo ""
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
