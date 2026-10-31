#!/bin/sh
# run-spawn-pipe.sh : the SKJ_SYS_PIPE + fd-passing demo.
#
# A root creates a pipe and spawns two children: a writer with the pipe's write
# end as fd 1 and a reader with the pipe's read end as fd 0, placed through the
# spawn fd map.  The writer's message travels the pipe to the reader, which
# echoes it, so the host sees "piped-hello" and then "parent done".
#
# Both guests link the fs/-interpose libc (for pipe(), fs_fd_backing, and the
# fd-space read/write).  Env knobs mirror run-spawn-mounts.sh.

set -eu

HERE=$(dirname "$0")
ROOT=$(cd "$HERE/.." && pwd)
CCBIN="${CCBIN:?CCBIN required}"
ASM="${ASM:?ASM required}"
LD="${LD:?LD required}"
START="${START:?START required}"
HALF="${HALF-}"
OUTDIR="${OUTDIR:?OUTDIR required}"
RUN="${RUN:?RUN required}"
NAME="${NAME:-test-spawn-pipe}"

CFLAGS="-DSKJ_LIBC_FLOAT -DSKJ_FS_INTERPOSE -DFS_WITH_AMBIENT -DFS_WITH_HOSTCAP -DFS_WITH_STREAMS -I$ROOT/fs"

LIBC="$ROOT/libc/string.c $ROOT/libc/ctype.c $ROOT/libc/stdlib.c \
$ROOT/libc/errno.c $ROOT/libc/malloc.c $ROOT/libc/sbrk.c \
$ROOT/libc/fdio.c $ROOT/fs/fs.c $ROOT/fs/ramfs.c $ROOT/fs/ambientfs.c \
$ROOT/fs/hostcap.c"

mkdir -p "$OUTDIR"

libc_objs=""
for src in $LIBC; do
    name=$(basename "$src" .c)
    # shellcheck disable=SC2086
    "$CCBIN" $CFLAGS -o "$OUTDIR/sp_$name.s" "$src"
    $ASM "$OUTDIR/sp_$name.o" "$OUTDIR/sp_$name.s"
    libc_objs="$libc_objs $OUTDIR/sp_$name.o"
done

build_elf() {
    src="$1"; out="$2"; extra_inc="${3-}"
    name=$(basename "$src" .c)
    # shellcheck disable=SC2086
    "$CCBIN" $CFLAGS $extra_inc -o "$OUTDIR/$name.s" "$src"
    $ASM "$OUTDIR/$name.o" "$OUTDIR/$name.s"
    # shellcheck disable=SC2086
    $LD "$out" "$START" "$OUTDIR/$name.o" $libc_objs $HALF
}

build_elf "$ROOT/tests/spawn-pipe/child.c" "$OUTDIR/child.elf"
{
    printf 'static const unsigned char child_img[] = {\n'
    xxd -i < "$OUTDIR/child.elf"
    printf '};\n#define CHILD_IMG_LEN %uu\n' "$(wc -c < "$OUTDIR/child.elf")"
} > "$OUTDIR/child_image.h"

build_elf "$ROOT/tests/spawn-pipe/parent.c" "$OUTDIR/parent.elf" "-I$OUTDIR"

tmp=$(mktemp -d)
"$RUN" "$OUTDIR/parent.elf" >"$tmp/out" 2>"$tmp/err" || true

fail=0
grep -q "piped-hello" "$tmp/out" || { echo "FAIL: the piped message did not reach the reader"; fail=1; }
grep -q "parent done" "$tmp/out" || { echo "FAIL: parent did not finish waiting on both children"; fail=1; }

if [ "$fail" -ne 0 ]; then
    echo "--- stdout ---"; cat "$tmp/out"
    echo "--- stderr ---"; cat "$tmp/err"
    rm -rf "$tmp"
    echo "$NAME: FAILED"
    exit 1
fi
rm -rf "$tmp"
echo "$NAME: a pipe carried a message between two spawned children"
exit 0
