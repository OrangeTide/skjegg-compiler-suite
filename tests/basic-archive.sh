#!/bin/sh
# basic-archive.sh : build a BASIC module archive (.a).
#
# A BASIC module is a GNU `ar` archive carrying its interface (the `.bmi` the
# front end reads to resolve a `FROM M IMPORT`) in a reserved member named
# `.bmi`, beside the compiled `.o` members a linker pulls.  This stages the
# interface under the reserved short name (short so it never routes through the
# "//" long-name table) and archives it, with any objects, via skj-ar.  It is the
# BASIC counterpart of kp-archive.sh.
#
#   basic-archive.sh AR OUT.a META.bmi [OBJ.o ...]
#
#   AR         the skj-ar to use
#   OUT.a      the module archive to (re)create
#   META.bmi   the .bmi interface payload (stored as the reserved `.bmi` member)
#   OBJ.o      zero or more compiled objects (the module's code)
set -e

AR=$1
OUT=$2
META=$3
shift 3

d=$(mktemp -d)
trap 'rm -rf "$d"' EXIT

cp "$META" "$d/.bmi"
rm -f "$OUT"
"$AR" rcs "$OUT" "$d/.bmi" "$@"
