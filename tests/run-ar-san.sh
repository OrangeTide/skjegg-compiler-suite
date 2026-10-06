#!/bin/sh
# run-ar-san.sh - sanitizer probe for skj-ar over its real operations.
#
# skj-ar is target-neutral: it parses each member's ELF32 symbol table
# (either endianness) for the index and copies member bytes verbatim. The
# probe exercises the three operations the tool implements (create with a
# symbol index, list, extract) over real object files passed on the command
# line, so the ELF32 parse, the GNU string table, and the symbol-index build
# all run under the sanitizers. A nonzero exit is a failure; the extracted
# members are compared byte-for-byte against their inputs.
#
# Usage: run-ar-san.sh <object>...
#   AR  the sanitized skj-ar binary (default build/skj-ar)

set -u

HERE=$(dirname "$0")
ROOT=$(cd "$HERE/.." && pwd)
AR=${AR:-"$ROOT/build/skj-ar"}
WORK="${WORK:-$ROOT/build/san/ar-work}"

rm -rf "$WORK"
mkdir -p "$WORK"

arch="$WORK/lib.a"
fail=0

# create with a symbol index (the s of rcs)
if ! "$AR" rcs "$arch" "$@"; then
    echo "FAIL (ar): create"
    exit 1
fi

# list
if ! "$AR" t "$arch" >"$WORK/list.txt"; then
    echo "FAIL (ar): list"
    exit 1
fi

# extract into a clean dir and compare each member to its source
( cd "$WORK" && "$AR" x "$arch" ) || { echo "FAIL (ar): extract"; exit 1; }
for obj in "$@"; do
    base=$(basename "$obj")
    if ! cmp -s "$obj" "$WORK/$base"; then
        echo "FAIL (ar): extracted $base differs from input"
        fail=$((fail + 1))
    fi
done

if [ "$fail" -eq 0 ]; then
    echo "ar: create/list/extract clean on $# member(s)"
fi
[ "$fail" -eq 0 ]
