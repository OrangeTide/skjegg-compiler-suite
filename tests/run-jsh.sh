#!/bin/sh
# run-jsh.sh : the jsh shell + coreutils end-to-end demo.
#
# Builds the coreutils (cat, echo, ls, rm, sleep, cp, mv) as guest ELFs, packs
# them into a romfs mounted at bin:, builds jsh against skjegg's fs-interpose
# libc, and runs a short script through it under skj-run.  jsh resolves a bare
# command name to bin:<name>, reads the program image, and spawns a fresh
# sandbox for it, wiring pipes and redirects through the spawn fd map.  A
# host directory is granted at data: (and inherited by every spawned child via
# SKJ_SPAWN_FLAG_SHARE_MOUNTS), so the file commands have somewhere to work.
#
# Env knobs mirror run-spawn-pipe.sh, plus MKROMFS (the host packer).

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
MKROMFS="${MKROMFS:?MKROMFS required}"
NAME="${NAME:-test-jsh}"

CFLAGS="-DSKJ_LIBC_FLOAT -DSKJ_FS_INTERPOSE -DFS_WITH_AMBIENT -DFS_WITH_HOSTCAP \
-DFS_WITH_ROM -DFS_WITH_STREAMS -I$ROOT/include -I$ROOT/libc -I$ROOT/fs"

# The fs-interpose libc: the path/fd/dir/stat calls route through the mount
# table, so stat, unlink, rename and readdir reach the mounts (not the host).
LIBC="string ctype stdlib printf numfmt stdio_core stdio malloc sbrk errno \
strerror getenv fdio dirent stat fileops fini"
FSLIB="fs ramfs ambientfs hostcap romfs"

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

# Build one guest ELF from a single C source plus the shared libc.
build_elf() {
    src="$1"; out="$2"; extra="${3-}"
    base=$(basename "$src" .c)
    # shellcheck disable=SC2086
    "$CCBIN" $CFLAGS $extra -o "$OUTDIR/$base.s" "$src"
    $ASM "$OUTDIR/$base.o" "$OUTDIR/$base.s"
    # shellcheck disable=SC2086
    $LD "$out" "$START" "$OUTDIR/$base.o" $libc_objs $HALF
}

# The coreutils, staged under bin/ by their bare names so bin:<name> resolves.
mkdir -p "$OUTDIR/bin"
for tool in cat echo ls rm sleep cp mv; do
    build_elf "$ROOT/examples/bin/$tool.c" "$OUTDIR/bin/$tool"
done
"$MKROMFS" "$OUTDIR/bin" "$OUTDIR/bin.img"

# The shell itself.
build_elf "$ROOT/examples/jsh/jsh.c" "$OUTDIR/jsh.elf" "-DSKJEGG"

# A tier that cannot run its guest here (an aarch64 HV guest off an aarch64 KVM
# host) still verifies the compile/assemble/link path.  BUILD_ONLY stops here.
if [ "${BUILD_ONLY:-}" = "1" ]; then
    echo "$NAME: built jsh + coreutils (build-only, guest not run here)"
    exit 0
fi

# A writable host directory for the data: mount.
seed="$OUTDIR/data"
rm -rf "$seed"
mkdir -p "$seed"

tmp=$(mktemp -d)
"$RUN" --rom "bin:$OUTDIR/bin.img" --mount "data:$seed" \
    "$OUTDIR/jsh.elf" <"$ROOT/tests/jsh/demo.jsh" >"$tmp/out" 2>"$tmp/err" || true

fail=0
grep -q "hello-from-jsh" "$tmp/out" || { echo "FAIL: a simple spawned echo did not run"; fail=1; }
grep -q "piped-hello" "$tmp/out"   || { echo "FAIL: the echo | cat pipeline did not carry its message"; fail=1; }
grep -q "to-a-file" "$tmp/out"     || { echo "FAIL: redirect to a file then cat did not round-trip"; fail=1; }
grep -q "jsh-out.txt" "$tmp/out"   || { echo "FAIL: ls did not list the file the redirect created"; fail=1; }
# The data: mount is a real host directory, so verify the redirect truly wrote
# the file rather than echo's output merely leaking to the terminal.
grep -q "to-a-file" "$seed/jsh-out.txt" 2>/dev/null \
    || { echo "FAIL: the redirect did not write the file on the data: mount"; fail=1; }

if [ "$fail" -ne 0 ]; then
    echo "--- stdout ---"; cat "$tmp/out"
    echo "--- stderr ---"; cat "$tmp/err"
    rm -rf "$tmp"
    echo "$NAME: FAILED"
    exit 1
fi
rm -rf "$tmp"
echo "$NAME: jsh spawned coreutils, piped between them, and redirected to a mount"
exit 0
