#!/bin/sh
# run-libc-tests.sh - compile the libc sources once, then build, link, and run
# each self-checking test program with one skj-cc target under qemu, checking
# the exit code (42 on success).
#
# Target is selected by environment, the same knobs as run-cc-tests.sh:
#   CCBIN  - the C compiler        (default build/skj-cc)
#   ASM    - assembler + "-o"      (default "build/skj-as -o")
#   LD     - linker + "-o"         (default "m68k-linux-gnu-ld -o")
#   QEMU   - qemu user-mode        (default qemu-m68k)
#   START  - crt object            (default build/start.o)
#   HALF   - extra runtime objects (default build/half.o; the _Float16 and
#            varargs helpers a target needs, empty to omit)
#   OUTDIR - where .s/.o/binary land (default build)
#   TEST   - test sources, space-separated (default the Phase A and B tests)
#   LIBC   - the libc sources      (default the implemented set)
#   CFLAGS - extra skj-cc flags     (default -DSKJ_LIBC_FLOAT: the printf float
#            conversions are a compile-time option, and this harness runs under
#            qemu where double works, so it enables them to cover the %f tests)
#   EXPECT - expected exit code     (default 42)

set -eu

HERE=$(dirname "$0")
ROOT=$(cd "$HERE/.." && pwd)
CCBIN="${CCBIN:-$ROOT/build/skj-cc}"
ASM="${ASM:-$ROOT/build/skj-as -o}"
LD="${LD:-m68k-linux-gnu-ld -o}"
# An explicitly empty QEMU means "run natively"; only an unset QEMU defaults.
QEMU="${QEMU-qemu-m68k}"
START="${START-$ROOT/build/start.o}"
HALF="${HALF-$ROOT/build/half.o}"
OUTDIR="${OUTDIR:-$ROOT/build}"
TEST="${TEST:-$ROOT/tests/libc_phase_a.c $ROOT/tests/libc_phase_b.c $ROOT/tests/libc_phase_c.c $ROOT/tests/libc_phase_e.c $ROOT/tests/libc_phase_f.c $ROOT/tests/libc_phase_g.c $ROOT/tests/libc_phase_h.c $ROOT/tests/libc_coroutine.c $ROOT/tests/libc_threads_stress.c $ROOT/tests/libc_statx.c $ROOT/tests/libc_pathops.c $ROOT/tests/libc_readdir.c}"
# runtime/rng.c holds the shared generators rand/srand draw from; it is pure
# (no syscalls) and links on every target.  libc/arc4random.c is the CSPRNG
# surface and needs the getrandom seam wrapper, which only the RV32 and ColdFire
# crts carry, so it is added only when ARC4RANDOM is set (the cf/rv targets).
LIBC_DEFAULT="$ROOT/libc/string.c $ROOT/libc/ctype.c $ROOT/libc/stdlib.c $ROOT/libc/printf.c $ROOT/libc/numfmt.c $ROOT/libc/stdio_core.c $ROOT/libc/stdio.c $ROOT/libc/assert.c $ROOT/libc/malloc.c $ROOT/libc/sbrk.c $ROOT/libc/errno.c $ROOT/libc/strerror.c $ROOT/libc/rand.c $ROOT/runtime/rng.c $ROOT/libc/strtod.c $ROOT/libc/scanf.c $ROOT/libc/coro.c $ROOT/libc/threads.c $ROOT/libc/stat.c $ROOT/libc/fileops.c $ROOT/libc/dirent.c $ROOT/libc/fini.c"
if [ -n "${ARC4RANDOM:-}" ]; then
    LIBC_DEFAULT="$LIBC_DEFAULT $ROOT/libc/arc4random.c"
fi
# FS_INTERPOSE routes the libc fd model (open/read/write/lseek/close and the
# path and directory ops) through the shared fs/ layer with the temporary
# ambient host backend.  Selected on the freestanding VM tiers (rv, cf).
if [ -n "${FS_INTERPOSE:-}" ]; then
    LIBC_DEFAULT="$LIBC_DEFAULT $ROOT/libc/fdio.c $ROOT/fs/fs.c $ROOT/fs/ramfs.c $ROOT/fs/ambientfs.c $ROOT/fs/hostcap.c"
    # Optional extra backends, each selected by its own knob (devfs needs no
    # host resource; romfs/9pfs add their guest-side pieces).
    [ -n "${FS_DEV:-}" ] && LIBC_DEFAULT="$LIBC_DEFAULT $ROOT/fs/devfs.c"
    [ -n "${FS_ROM:-}" ] && LIBC_DEFAULT="$LIBC_DEFAULT $ROOT/fs/romfs.c"
    [ -n "${FS_9P:-}" ] && LIBC_DEFAULT="$LIBC_DEFAULT $ROOT/fs/ninefs.c $ROOT/fs/p9_guest.c"
fi
LIBC="${LIBC:-$LIBC_DEFAULT}"
CFLAGS="${CFLAGS--DSKJ_LIBC_FLOAT}"
if [ -n "${FS_INTERPOSE:-}" ]; then
    CFLAGS="$CFLAGS -DSKJ_FS_INTERPOSE -DFS_WITH_AMBIENT -DFS_WITH_HOSTCAP -I$ROOT/fs"
    [ -n "${FS_DEV:-}" ] && CFLAGS="$CFLAGS -DFS_WITH_DEV"
    [ -n "${FS_ROM:-}" ] && CFLAGS="$CFLAGS -DFS_WITH_ROM"
    [ -n "${FS_9P:-}" ] && CFLAGS="$CFLAGS -DFS_WITH_9P"
fi
# FS_AMBIENT_DEFAULT makes fs_init land a no-mount guest on the unconfined
# ambient host mount (the pre-fs bare-path-reaches-host behaviour), rather than
# the scratch ramfs that is now the default.  The file-op suites set it because
# they open host files by bare path under qemu with no launcher to grant a
# mount; the capability test deliberately leaves it off to prove the folded-out
# default (scratch landing, host only through a named grant).
if [ -n "${FS_AMBIENT_DEFAULT:-}" ]; then
    CFLAGS="$CFLAGS -DFS_AMBIENT_DEFAULT"
fi
EXPECT="${EXPECT:-42}"
mkdir -p "$OUTDIR"

if [ ! -x "$CCBIN" ]; then
    echo "ERROR: $CCBIN not found"
    exit 1
fi

# Compile and assemble the libc once; it is shared by every test binary.
# Each compile names its source first, and a failing one is reported and
# skipped rather than aborting the run, so the log names every source the
# compiler rejected (the diagnostic a CI-only failure needs).
libc_objs=""
libc_fail=0
for src in $LIBC; do
    name=$(basename "$src" .c)
    asm="$OUTDIR/libc_$name.s"
    obj="$OUTDIR/libc_$name.o"
    # shellcheck disable=SC2086
    if ! "$CCBIN" $CFLAGS -o "$asm" "$src"; then
        echo "FAIL  libc compile: $src"
        libc_fail=1
        continue
    fi
    $ASM "$obj" "$asm"
    libc_objs="$libc_objs $obj"
done
if [ "$libc_fail" -ne 0 ]; then
    echo "libc: the compiler rejected the source(s) named above"
    exit 1
fi

status=0
for test in $TEST; do
    name=$(basename "$test" .c)
    asm="$OUTDIR/$name.s"
    obj="$OUTDIR/$name.o"
    bin="$OUTDIR/$name"
    # shellcheck disable=SC2086
    if ! "$CCBIN" $CFLAGS -o "$asm" "$test"; then
        echo "FAIL  test compile: $test"
        status=1
        continue
    fi
    $ASM "$obj" "$asm"
    # START empty (the macOS/libSystem lane: the system crt supplies the entry
    # and libSystem the C library, so no in-tree crt or libc objects are linked)
    # is omitted rather than passed as an empty argument.
    # shellcheck disable=SC2086
    if [ -n "$START" ]; then
        $LD "$bin" "$START" "$obj" $libc_objs $HALF
    else
        $LD "$bin" "$obj" $libc_objs $HALF
    fi

    # BUILD_ONLY proves the compile/assemble/link path on a host that cannot run
    # the result (the aarch64 HV guest needs an aarch64 KVM host).
    if [ -n "${BUILD_ONLY:-}" ]; then
        printf 'PASS  libc %s (%s, build)\n' \
            "$(echo "$name" | sed 's/^libc_//')" "$(basename "$CCBIN")"
        continue
    fi

    # A sibling .input is fed as stdin; a sibling .expected is diffed against
    # stdout.  Both are optional, so a self-checking exit-code test needs
    # neither.
    src_dir=$(dirname "$test")
    input="$src_dir/$name.input"
    expected="$src_dir/$name.expected"
    actual="$OUTDIR/$name.out"

    set +e
    if [ -f "$input" ]; then
        $QEMU "$bin" < "$input" > "$actual"
    else
        $QEMU "$bin" > "$actual"
    fi
    rc=$?
    set -e

    label=$(echo "$name" | sed 's/^libc_//')
    ok=1
    [ "$rc" -eq "$EXPECT" ] || ok=0
    if [ -f "$expected" ]; then
        if ! diff -u "$expected" "$actual" > "$OUTDIR/$name.diff" 2>&1; then
            ok=0
        fi
    fi
    if [ "$ok" -eq 1 ]; then
        printf 'PASS  libc %s (%s, exit %d)\n' "$label" "$(basename "$CCBIN")" "$rc"
    else
        printf 'FAIL  libc %s (%s, exit %d, want %d)\n' "$label" "$(basename "$CCBIN")" "$rc" "$EXPECT"
        [ -f "$expected" ] && [ -s "$OUTDIR/$name.diff" ] && cat "$OUTDIR/$name.diff"
        status=1
    fi
done

exit $status
