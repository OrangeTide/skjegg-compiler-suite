#!/bin/sh
# run-jsh.sh : the jsh shell + coreutils end-to-end demo.
#
# Builds the coreutils (cat, echo, ls, rm, sleep, cp, mv) as guest ELFs served
# as a read-only host directory mounted at bin:, builds jsh against skjegg's
# fs-interpose libc, and runs a short script through it under skj-run.  jsh
# resolves a bare command name to bin:<name>, reads the program image, spawns a
# fresh
# sandbox for it, wiring pipes and redirects through the spawn fd map.  A
# host directory is granted at data: (and inherited by every spawned child via
# SKJ_SPAWN_FLAG_SHARE_MOUNTS), so the file commands have somewhere to work.
#
# Env knobs mirror run-spawn-pipe.sh.

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
NAME="${NAME:-test-jsh}"

CFLAGS="-DSKJ_LIBC_FLOAT -DSKJ_FS_INTERPOSE -DFS_WITH_AMBIENT -DFS_WITH_HOSTCAP \
-DFS_WITH_STREAMS -I$ROOT/include -I$ROOT/libc -I$ROOT/fs"

# The fs-interpose libc: the path/fd/dir/stat calls route through the mount
# table, so stat, unlink, rename and readdir reach the mounts (not the host).
# No FS_WITH_RAM and no FS_WITH_ROM: skj-run owns both filesystems.  It grants an
# ephemeral host "scratch" directory for the default landing spot and serves the
# coreutils as a read-only host "bin" mount, so the guest drops its in-guest
# ramfs and romfs entirely and every backend it uses is a host capability.
LIBC="string ctype stdlib printf numfmt stdio_core stdio malloc sbrk errno \
strerror getenv fdio dirent stat fileops fini"
FSLIB="fs ambientfs hostcap"

mkdir -p "$OUTDIR"

# Build the libc + fs-interpose sources and archive them with a symbol index.
# Linking against the archive (not the loose objects) means a guest pulls only
# the members that resolve a symbol it actually references, the classic ar(1)
# dead-strip: a program that never formats does not drag printf's object in.
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

# Build one guest ELF from a single C source plus the shared libc archive.
build_elf() {
    src="$1"; out="$2"; extra="${3-}"
    base=$(basename "$src" .c)
    # shellcheck disable=SC2086
    "$CCBIN" $CFLAGS $extra -o "$OUTDIR/$base.s" "$src"
    $ASM "$OUTDIR/$base.o" "$OUTDIR/$base.s"
    # shellcheck disable=SC2086
    $LD "$out" "$START" "$OUTDIR/$base.o" "$OUTDIR/libc.a" $HALF
}

# The coreutils, staged under bin/ by their bare names so bin:<name> resolves.
# This host directory is served directly as the read-only bin: mount below, so
# there is no romfs image to pack: one host copy serves jsh and every child.
mkdir -p "$OUTDIR/bin"
for tool in cat echo ls rm sleep cp mv; do
    build_elf "$ROOT/examples/bin/$tool.c" "$OUTDIR/bin/$tool"
done

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
# Bound the run with a timeout when one is available, as a generic safety net:
# the HV cores run the shell and its children as several guests round-robin on
# one host thread, and a blocking pipe read or write now parks cooperatively
# (the scheduler runs the sibling instead of stalling the shared thread), but if
# that ever regressed a wedged guest would block make indefinitely.  With the
# guard the greps below fail and the test reports a bounded failure instead.
if command -v timeout >/dev/null 2>&1; then
    RUN_GUARD="timeout 60"
else
    RUN_GUARD=""
fi
$RUN_GUARD "$RUN" --mount-ro "bin:$OUTDIR/bin" --mount "data:$seed" \
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
