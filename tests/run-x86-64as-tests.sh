#!/bin/sh
# x86-64 assembler encoding test runner.
#
# For each enc_*.s: assemble with skj-as-x86-64 and with the GNU assembler in
# intel mode (as --64), then compare the .text/.rodata/.data section bytes and
# the relocations.  GNU as is the golden master, so a mismatch is a
# skj-as-x86-64 bug.
#
# Relocations are compared as a normalized set (offset, type, symbol name,
# addend) sorted by offset, not as a textual readelf dump: the two assemblers
# order the relocation table and number the symbol table differently, which is
# not a correctness difference.
#
# The corpus deliberately targets external symbols for branches, so GNU as emits
# the same rel32 form skj-as-x86-64 always uses (the assembler does not relax a
# near branch to rel8; see as/x86_64.h).
#
# Tools are overridable by environment variable.

DIR=$(dirname "$0")/x86-64as
SKJ=${SKJ:-./build/skj-as-x86-64}
GAS=${GAS:-as}
GASFLAGS=${GASFLAGS:---64}
OBJCOPY=${OBJCOPY:-objcopy}
READELF=${READELF:-readelf}

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
pass=0
fail=0

extract() {
    "$OBJCOPY" -O binary --only-section="$2" "$1" "$3" 2>/dev/null || :
    [ -f "$3" ] || : > "$3"
}

# normalized relocation set: offset, type, symbol, +/-, addend, sorted
relocs() {
    "$READELF" -r "$1" 2>/dev/null | awk '/R_X86_64/{print $1, $3, $5, $6, $7}' | sort
}

echo "== encoding tests (golden master: $GAS $GASFLAGS) =="
for s in "$DIR"/enc_*.s; do
    [ -f "$s" ] || continue
    name=$(basename "$s" .s)
    if ! "$SKJ" -o "$TMP/$name.skj.o" "$s" 2>"$TMP/err"; then
        echo "FAIL $name (skj-as-x86-64 error)"; sed 's/^/    /' "$TMP/err"
        fail=$((fail + 1)); continue
    fi
    if ! "$GAS" $GASFLAGS -o "$TMP/$name.gas.o" "$s" 2>"$TMP/err"; then
        echo "SKIP $name (GNU as could not assemble it)"; continue
    fi
    ok=1
    for sec in .text .rodata .data; do
        extract "$TMP/$name.skj.o" "$sec" "$TMP/$name.skj$sec"
        extract "$TMP/$name.gas.o" "$sec" "$TMP/$name.gas$sec"
        cmp -s "$TMP/$name.skj$sec" "$TMP/$name.gas$sec" || ok=0
    done
    relocs "$TMP/$name.skj.o" > "$TMP/$name.skjrel"
    relocs "$TMP/$name.gas.o" > "$TMP/$name.gasrel"
    cmp -s "$TMP/$name.skjrel" "$TMP/$name.gasrel" || ok=0
    if [ "$ok" = 1 ]; then
        echo "PASS $name"; pass=$((pass + 1))
    else
        echo "FAIL $name (mismatch vs GNU as)"; fail=$((fail + 1))
    fi
done

echo ""
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
