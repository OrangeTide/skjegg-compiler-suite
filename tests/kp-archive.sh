#!/bin/sh
# kp-archive.sh : build a Kobold unit archive (.a).
#
# A Kobold unit is a GNU `ar` archive carrying its interface (the KPM payload the
# front end reads to resolve a `uses`) in a reserved member named `.kpm`, beside
# any compiled `.o` members a linker pulls.  This stages the interface under the
# reserved short name (short so it never routes through the "//" long-name table)
# and archives it, with any objects, via skj-ar.
#
#   kp-archive.sh AR OUT.a META.kpm [OBJ.o ...]
#
#   AR         the skj-ar to use
#   OUT.a      the unit archive to (re)create
#   META.kpm   the KPM interface payload (stored as the reserved `.kpm` member)
#   OBJ.o      zero or more compiled objects (the unit's code)
set -e

AR=$1
OUT=$2
META=$3
shift 3

d=$(mktemp -d)
trap 'rm -rf "$d"' EXIT

cp "$META" "$d/.kpm"
rm -f "$OUT"
"$AR" rcs "$OUT" "$d/.kpm" "$@"
