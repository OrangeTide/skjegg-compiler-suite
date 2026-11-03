#!/bin/sh
# run-hv-pump.sh : the cooperative-park stress test for the HV cores.
#
# The parent creates a pipe and spawns a writer with the write end as fd 1 and a
# reader with the read end as fd 0; the hv-pump child moves a payload several
# pipe buffers large and yields and sleeps mid-transfer.  The writer and reader
# are two guests round-robin on one host thread, so a full-pipe write (POLLOUT),
# an empty-pipe read (POLLIN), a sched_yield, and a nanosleep each have to park
# the caller and let the sibling run, rather than block the shared thread.  The
# whole payload crosses and the reader verifies it byte for byte.
#
# Env knobs mirror run-jsh.sh (START/HALF carry the HV crt).

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
NAME="${NAME:-test-hv-pump}"

CFLAGS="-DSKJ_LIBC_FLOAT -DSKJ_FS_INTERPOSE -DFS_WITH_AMBIENT -DFS_WITH_HOSTCAP -DFS_WITH_STREAMS -DFS_WITH_RAM -I$ROOT/fs"

LIBC="$ROOT/libc/string.c $ROOT/libc/ctype.c $ROOT/libc/stdlib.c \
$ROOT/libc/errno.c $ROOT/libc/malloc.c $ROOT/libc/sbrk.c \
$ROOT/libc/fdio.c $ROOT/fs/fs.c $ROOT/fs/ramfs.c $ROOT/fs/ambientfs.c \
$ROOT/fs/hostcap.c"

mkdir -p "$OUTDIR"

libc_objs=""
for src in $LIBC; do
    name=$(basename "$src" .c)
    # shellcheck disable=SC2086
    "$CCBIN" $CFLAGS -o "$OUTDIR/hp_$name.s" "$src"
    $ASM "$OUTDIR/hp_$name.o" "$OUTDIR/hp_$name.s"
    libc_objs="$libc_objs $OUTDIR/hp_$name.o"
done

build_elf() {
    src="$1"; out="$2"; extra="${3-}"
    name=$(basename "$src" .c)
    # shellcheck disable=SC2086
    "$CCBIN" $CFLAGS $extra -o "$OUTDIR/$name.s" "$src"
    $ASM "$OUTDIR/$name.o" "$OUTDIR/$name.s"
    # shellcheck disable=SC2086
    $LD "$out" "$START" "$OUTDIR/$name.o" $libc_objs $HALF
}

# The large-transfer child, baked into the parent as a spawnable image.
PUMP_BYTES="${PUMP_BYTES:-262144}"
build_elf "$ROOT/tests/hv-pump/child.c" "$OUTDIR/child.elf" "-DPUMP_BYTES=${PUMP_BYTES}u"
{
    printf 'static const unsigned char child_img[] = {\n'
    xxd -i < "$OUTDIR/child.elf"
    printf '};\n#define CHILD_IMG_LEN %uu\n' "$(wc -c < "$OUTDIR/child.elf")"
} > "$OUTDIR/child_image.h"

# The LP64-safe parent wires the pipe and spawns the writer ("w") and reader
# ("r"), which the hv-pump child implements.
build_elf "$ROOT/tests/hv-pump/parent.c" "$OUTDIR/parent.elf" "-I$OUTDIR"

# Bound the run: if a park regresses, a guest wedges, and the timeout turns an
# indefinite hang into a bounded failure.
if command -v timeout >/dev/null 2>&1; then
    RUN_GUARD="timeout 60"
else
    RUN_GUARD=""
fi

tmp=$(mktemp -d)
$RUN_GUARD "$RUN" "$OUTDIR/parent.elf" >"$tmp/out" 2>"$tmp/err" || true

fail=0
grep -q "pump-ok" "$tmp/out"     || { echo "FAIL: the large payload did not cross the pipe intact"; fail=1; }
grep -q "parent done" "$tmp/out" || { echo "FAIL: parent did not finish waiting on both children"; fail=1; }

if [ "$fail" -ne 0 ]; then
    echo "--- stdout ---"; cat "$tmp/out"
    echo "--- stderr ---"; cat "$tmp/err"
    rm -rf "$tmp"
    echo "$NAME: FAILED"
    exit 1
fi
rm -rf "$tmp"
echo "$NAME: full-pipe write, empty-pipe read, yield, and sleep all parked cooperatively"
exit 0
