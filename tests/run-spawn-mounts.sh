#!/bin/sh
# run-spawn-mounts.sh : the SKJ_SPAWN_FLAG_SHARE_MOUNTS demo.
#
# A root process (the parent) is granted a "data" capability mount with
# --mount.  It spawns a child with SKJ_SPAWN_FLAG_SHARE_MOUNTS, so the child
# inherits that mount and reads a seeded file through it.  The host therefore
# sees the file content and the parent's "child status=0" line.
#
# Both guests link the fs/-interpose libc: the parent so the root image exports
# the fs_mounts symbol the launcher fills, the child so it can open the mount.
# The build mirrors run-libc-tests.sh's knobs (CCBIN/ASM/LD/START/HALF).
#
# Env knobs:
#   CCBIN  - the C compiler
#   ASM    - assembler + "-o"
#   LD     - linker + "-o"
#   START  - crt object
#   HALF   - extra runtime objects
#   OUTDIR - build directory
#   RUN    - skj-run binary
#   NAME   - test name for the pass/fail line

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
NAME="${NAME:-test-spawn-mounts}"

CFLAGS="-DSKJ_LIBC_FLOAT -DSKJ_FS_INTERPOSE -DFS_WITH_AMBIENT -DFS_WITH_HOSTCAP -DFS_WITH_STREAMS -I$ROOT/fs"

LIBC="$ROOT/libc/string.c $ROOT/libc/ctype.c $ROOT/libc/stdlib.c \
$ROOT/libc/errno.c $ROOT/libc/malloc.c $ROOT/libc/sbrk.c \
$ROOT/libc/fdio.c $ROOT/fs/fs.c $ROOT/fs/ramfs.c $ROOT/fs/ambientfs.c \
$ROOT/fs/hostcap.c"

mkdir -p "$OUTDIR"

# The data directory the launcher grants, with the file the child reads back.
DATA="$OUTDIR/data"
rm -rf "$DATA"
mkdir -p "$DATA"
printf 'capability-granted\n' > "$DATA/seed.txt"

# Compile the shared libc/fs objects once.
libc_objs=""
for src in $LIBC; do
    name=$(basename "$src" .c)
    # shellcheck disable=SC2086
    "$CCBIN" $CFLAGS -o "$OUTDIR/sm_$name.s" "$src"
    $ASM "$OUTDIR/sm_$name.o" "$OUTDIR/sm_$name.s"
    libc_objs="$libc_objs $OUTDIR/sm_$name.o"
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

# Child first, then embed it as a C array the parent includes.
build_elf "$ROOT/tests/spawn-mounts/child.c" "$OUTDIR/child.elf"
{
    printf 'static const unsigned char child_img[] = {\n'
    xxd -i < "$OUTDIR/child.elf"
    printf '};\n#define CHILD_IMG_LEN %uu\n' "$(wc -c < "$OUTDIR/child.elf")"
} > "$OUTDIR/child_image.h"

build_elf "$ROOT/tests/spawn-mounts/parent.c" "$OUTDIR/parent.elf" "-I$OUTDIR"

# Run the root under the launcher with the data mount granted.
tmp=$(mktemp -d)
"$RUN" --mount "data:$DATA" "$OUTDIR/parent.elf" >"$tmp/out" 2>"$tmp/err" || true
rc=$?

fail=0
grep -q "capability-granted"        "$tmp/out" || { echo "FAIL: child did not read the inherited mount"; fail=1; }
grep -q "parent: child status=0"    "$tmp/out" || { echo "FAIL: child did not exit 0 / parent lost status"; fail=1; }

if [ "$fail" -ne 0 ]; then
    echo "--- stdout ---"; cat "$tmp/out"
    echo "--- stderr ---"; cat "$tmp/err"
    rm -rf "$tmp"
    echo "$NAME: FAILED"
    exit 1
fi
rm -rf "$tmp"
echo "$NAME: child inherited the parent's mount and read through it"
exit 0
