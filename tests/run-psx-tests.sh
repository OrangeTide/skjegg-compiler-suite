#!/bin/sh
# run-psx-tests.sh : structural validation of a PS-EXE written by skj-ld-mips.
#
# No in-tree simulator loads a PS-EXE and models the R3051, so this validates
# the container the PlayStation BIOS loader reads (the 2048-byte "PS-X EXE"
# header and the flat image), and confirms the payload was relocated at the
# PlayStation RAM base rather than the Linux base.  Functional correctness of
# the code itself is covered by the ELF tier (check-cc-mips-skj), which runs the
# same relocated objects under qemu-mipsel.
#
# Usage: run-psx-tests.sh <file.exe>
# Env: OBJDUMP (default mipsel-none-elf-objdump)

set -e

EXE=$1
OBJDUMP=${OBJDUMP:-mipsel-none-elf-objdump}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

BASE=$((0x80010000))
STACK=$((0x801ffff0))

fail() { echo "FAIL: $1"; exit 1; }

# little-endian u32 at byte offset $1 (the host is little-endian, and so is the
# file, so od -tu4 reads the field directly)
u32() { od -An -tu4 -j "$1" -N4 "$EXE" | tr -d ' '; }

# 1. magic
magic=$(dd if="$EXE" bs=1 count=8 2>/dev/null)
[ "$magic" = "PS-X EXE" ] || fail "bad magic '$magic' (want 'PS-X EXE')"

pc0=$(u32 16)
t_addr=$(u32 24)
t_size=$(u32 28)
b_addr=$(u32 40)
b_size=$(u32 44)
s_addr=$(u32 48)

fsize=$(wc -c < "$EXE")

# 2. load address and stack are the PlayStation RAM constants
[ "$t_addr" -eq "$BASE" ] || fail "t_addr $t_addr != $BASE"
[ "$s_addr" -eq "$STACK" ] || fail "s_addr $s_addr != $STACK"

# 3. t_size is a whole number of 2048-byte sectors and matches the file
[ $((t_size % 2048)) -eq 0 ] || fail "t_size $t_size not a multiple of 2048"
[ $((2048 + t_size)) -eq "$fsize" ] || fail "file size $fsize != 2048 + t_size $t_size"

# 4. entry lies inside the loaded image
[ "$pc0" -ge "$t_addr" ] || fail "pc0 $pc0 below t_addr"
[ "$pc0" -lt $((t_addr + t_size)) ] || fail "pc0 $pc0 past the image"

# 5. bss lies at or above the end of the loaded image
if [ "$b_size" -ne 0 ]; then
    [ "$b_addr" -ge "$t_addr" ] || fail "b_addr $b_addr below t_addr"
fi

# 6. the payload was relocated at the PlayStation base: disassemble it raw and
#    confirm the jumps target 0x8001xxxx (PS1 RAM) and never 0x0040xxxx (the
#    Linux base the ELF path uses), so the writer used the PS-EXE layout.
dd if="$EXE" bs=2048 skip=1 of="$TMP/payload.bin" 2>/dev/null
"$OBJDUMP" -D -b binary -m mips:3000 -EL \
    --adjust-vma="$t_addr" "$TMP/payload.bin" > "$TMP/dis.txt" 2>/dev/null || \
    fail "could not disassemble the payload"

if grep -qE '\bjal\b' "$TMP/dis.txt"; then
    grep -qE '\bjal\b\s+0x8001' "$TMP/dis.txt" || \
        fail "no jal targets the PlayStation base (relocation wrong?)"
    if grep -qE '\bjal\b\s+0x0040' "$TMP/dis.txt"; then
        fail "a jal targets the Linux base 0x0040xxxx (wrong layout)"
    fi
fi

printf 'PASS: %s (PS-X EXE, t_addr=0x%08x t_size=%d pc0=0x%08x b_size=%d)\n' \
    "$(basename "$EXE")" "$t_addr" "$t_size" "$pc0" "$b_size"
