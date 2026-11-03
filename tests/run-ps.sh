#!/bin/sh
# run-ps.sh : the standalone `ps` tool over the proc: pseudo filesystem.
#
# Builds the fs-interpose libc and the ps coreutil as a guest ELF, then runs it
# under skj-run with no --mount at all: the proc: mount is always present, so ps
# can enumerate the process table and read each process's status.  Run as the
# sole (root) process, ps must list exactly itself: pid 1, state running.
#
# Env knobs mirror run-jsh.sh.

set -eu

HERE=$(dirname "$0")
ROOT=$(cd "$HERE/.." && pwd)
CCBIN="${CCBIN:?CCBIN required}"
ASM="${ASM:?ASM required}"
LD="${LD:?LD required}"
AR="${AR:?AR required}"
START="${START:?START required}"
HALF="${HALF-}"
OUTDIR="${OUTDIR:?OUTDIR required}"
RUN="${RUN:?RUN required}"
NAME="${NAME:-test-ps}"

CFLAGS="-DSKJ_LIBC_FLOAT -DSKJ_FS_INTERPOSE -DFS_WITH_AMBIENT -DFS_WITH_HOSTCAP \
-DFS_WITH_STREAMS -I$ROOT/include -I$ROOT/libc -I$ROOT/fs"

LIBC="string ctype stdlib printf numfmt stdio_core stdio malloc sbrk errno \
strerror getenv fdio dirent stat fileops fini"
FSLIB="fs ambientfs hostcap"

mkdir -p "$OUTDIR"

libc_objs=""
for name in $LIBC; do
    # shellcheck disable=SC2086
    "$CCBIN" $CFLAGS -o "$OUTDIR/l_$name.s" "$ROOT/libc/$name.c"
    $ASM "$OUTDIR/l_$name.o" "$OUTDIR/l_$name.s"
    libc_objs="$libc_objs $OUTDIR/l_$name.o"
done
for name in $FSLIB; do
    # shellcheck disable=SC2086
    "$CCBIN" $CFLAGS -o "$OUTDIR/f_$name.s" "$ROOT/fs/$name.c"
    $ASM "$OUTDIR/f_$name.o" "$OUTDIR/f_$name.s"
    libc_objs="$libc_objs $OUTDIR/f_$name.o"
done
rm -f "$OUTDIR/libc.a"
# shellcheck disable=SC2086
$AR rcs "$OUTDIR/libc.a" $libc_objs

"$CCBIN" $CFLAGS -o "$OUTDIR/ps.s" "$ROOT/examples/bin/ps.c"
$ASM "$OUTDIR/ps.o" "$OUTDIR/ps.s"
# shellcheck disable=SC2086
$LD "$OUTDIR/ps.elf" "$START" "$OUTDIR/ps.o" "$OUTDIR/libc.a" $HALF

if [ "${BUILD_ONLY:-}" = "1" ]; then
    echo "$NAME: built ps (build-only, guest not run here)"
    exit 0
fi

tmp=$(mktemp -d)
if command -v timeout >/dev/null 2>&1; then
    RUN_GUARD="timeout 60"
else
    RUN_GUARD=""
fi
$RUN_GUARD "$RUN" "$OUTDIR/ps.elf" >"$tmp/out" 2>"$tmp/err" || true

fail=0
grep -q "PID" "$tmp/out"       || { echo "FAIL: ps printed no header"; fail=1; }
grep -q "running" "$tmp/out"   || { echo "FAIL: ps did not list a running process"; fail=1; }
# The root process is pid 1 with no parent (printed as 0).
awk 'NR>1 && $1=="1" && $2=="running" && $3=="0" {found=1} END{exit !found}' \
    "$tmp/out" || { echo "FAIL: ps did not show pid 1 running with parent 0"; fail=1; }

if [ "$fail" -ne 0 ]; then
    echo "--- stdout ---"; cat "$tmp/out"
    echo "--- stderr ---"; cat "$tmp/err"
    rm -rf "$tmp"
    echo "$NAME: FAILED"
    exit 1
fi
rm -rf "$tmp"
echo "$NAME: ps lists the process table over proc: (pid 1 running)"
