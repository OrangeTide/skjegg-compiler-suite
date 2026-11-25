#!/bin/sh
# pascal-archive.sh : build a Pascal unit archive (.a).
#
# A Pascal unit is a GNU `ar` archive carrying its interface (the `.ppm` the
# front end reads to resolve a `uses`) in a reserved member named `.ppm`, beside
# the compiled `.o` members a linker pulls.  This stages the interface under the
# reserved short name (short so it never routes through the "//" long-name table)
# and archives it, with any objects, via skj-ar.  It is the Pascal counterpart of
# basic-archive.sh and kp-archive.sh.
#
#   pascal-archive.sh AR OUT.a META.ppm [OBJ.o ...]
#
#   AR         the skj-ar to use
#   OUT.a      the unit archive to (re)create
#   META.ppm   the .ppm interface payload (stored as the reserved `.ppm` member)
#   OBJ.o      zero or more compiled objects (the unit's code)
set -e

AR=$1
OUT=$2
META=$3
shift 3

d=$(mktemp -d)
trap 'rm -rf "$d"' EXIT

cp "$META" "$d/.ppm"
rm -f "$OUT"
"$AR" rcs "$OUT" "$d/.ppm" "$@"
