#!/bin/sh
# run-cc-tests.sh - compile, assemble, link, run each tests/cc_*.c under qemu
# and compare exit code against .exitcode file.
#
# Target is selected by environment (defaults to ColdFire):
#   CCBIN  - the C compiler        (default build/skj-cc)
#   ASM    - assembler + "-o"      (default "build/skj-as -o")
#   LD     - linker + "-o"         (default "m68k-linux-gnu-ld -o")
#   QEMU   - qemu user-mode        (default qemu-m68k)
#   START  - crt object            (default build/start.o)
#   HALF   - _Float16 helper obj   (default build/half.o; empty to omit, e.g.
#            on arm64 where the conversions are a native fcvt)
#   OUTDIR - where per-test .s/.o/binaries land (default build; use a private
#            dir for non-ColdFire targets so their binaries do not leak into
#            `make check`, which runs whatever build/<name> matches a fixture)
#   EXCLUDE - space-separated test names to skip (e.g. the float tests on a
#            backend without floating point)
# The build objects (START, HALF) are provided by the Makefile target.

set -eu

HERE=$(dirname "$0")
ROOT=$(cd "$HERE/.." && pwd)
CCBIN="${CCBIN:-$ROOT/build/skj-cc}"
ASM="${ASM:-$ROOT/build/skj-as -o}"
LD="${LD:-m68k-linux-gnu-ld -o}"
QEMU="${QEMU:-qemu-m68k}"
START="${START:-$ROOT/build/start.o}"
# no colon: an explicitly-empty HALF (e.g. arm64, native fcvt) stays empty
HALF="${HALF-$ROOT/build/half.o}"
OUTDIR="${OUTDIR:-$ROOT/build}"
EXCLUDE="${EXCLUDE:-}"
mkdir -p "$OUTDIR"

if [ ! -x "$CCBIN" ]; then
    echo "ERROR: $CCBIN not found"
    exit 1
fi

fail=0
pass=0

for src in "$HERE"/cc_*.c; do
    [ -f "$src" ] || continue
    name=$(basename "$src" .c)
    case " $EXCLUDE " in
        *" $name "*)
            printf 'SKIP  %s (excluded)\n' "$name"
            continue ;;
    esac
    exitcode_file="$HERE/$name.exitcode"
    if [ ! -f "$exitcode_file" ]; then
        printf 'SKIP  %s (no .exitcode)\n' "$name"
        continue
    fi

    expect_rc=$(cat "$exitcode_file")
    asm="$OUTDIR/$name.s"
    obj="$OUTDIR/$name.o"
    bin="$OUTDIR/$name"

    set +e
    "$CCBIN" -o "$asm" "$src" 2>/dev/null
    if [ $? -ne 0 ]; then
        printf 'FAIL  %s (compile error)\n' "$name"
        fail=$((fail + 1))
        set -e
        continue
    fi
    $ASM "$obj" "$asm" 2>/dev/null
    if [ $? -ne 0 ]; then
        printf 'FAIL  %s (assemble error)\n' "$name"
        fail=$((fail + 1))
        set -e
        continue
    fi
    $LD "$bin" "$START" "$obj" $HALF 2>/dev/null
    if [ $? -ne 0 ]; then
        printf 'FAIL  %s (link error)\n' "$name"
        fail=$((fail + 1))
        set -e
        continue
    fi
    "$QEMU" "$bin"
    rc=$?
    set -e

    if [ "$rc" -eq "$expect_rc" ]; then
        printf 'PASS  %s (exit %d)\n' "$name" "$rc"
        pass=$((pass + 1))
    else
        printf 'FAIL  %s (exit %d, want %d)\n' "$name" "$rc" "$expect_rc"
        fail=$((fail + 1))
    fi
done

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
