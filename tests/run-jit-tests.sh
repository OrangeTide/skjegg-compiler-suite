#!/bin/sh
# run-jit-tests.sh - JIT each tests/cc_*.c with skj-jit and compare the exit
# code against the .exitcode file.  The in-process counterpart of
# run-cc-tests.sh: no assembler, linker, or qemu, since skj-jit runs the
# compiled program in its own process.
#
#   JIT    - the jit driver (default build/skj-jit)
#
# The JIT covers the integer, float (single and double), control-flow, call
# (register and stack arguments, small-struct by value, varargs), global,
# sub-word, and unsigned-divide core of C.  Tests that use a feature the JIT
# does not implement yet are listed in EXCLUDE below, grouped by the missing
# feature, so this suite stays green while the gaps are tracked.

set -eu

HERE=$(dirname "$0")
ROOT=$(cd "$HERE/.." && pwd)
JIT="${JIT:-$ROOT/build/skj-jit}"

if [ ! -x "$JIT" ]; then
    echo "ERROR: $JIT not found"
    exit 1
fi

# Tests excluded because the JIT does not yet cover the feature they exercise:
#   inline assembly (IR_ASM): the AOT prints the asm string verbatim and leaves
#   it to an external gas/nasm, but the JIT emits bytes directly and has no
#   downstream assembler.  There is no in-tree x86-64 assembler to reuse (the
#   skj-as-* tools are ColdFire/RISC-V/MIPS, and emit_x86.c is a structured byte
#   encoder, not a text parser), so real support means writing a full x86-64
#   assembler (and finishing cc's extended asm, since bare asm without operands
#   is nearly all one can write today).  That is a large, self-contained piece:
#   if built, it should be build-time optional (default off) so the JIT stays
#   lean for the common case of C that never writes asm.
EXCLUDE="cc_t077_inline_asm"
#   _Float16 (IR_FLH / IR_FSH): deliberately deferred.  The encoding work is
#   small (bind the half.c softfloat helpers as imports, like __va_arg), but
#   there are several common 16-bit float formats (IEEE binary16, bfloat16),
#   so what a _Float16 should mean here is a design question to settle when a
#   real consumer needs it, not today.
EXCLUDE="$EXCLUDE cc_t059_float16"

fail=0
pass=0
skip=0

for src in "$HERE"/cc_*.c; do
    [ -f "$src" ] || continue
    name=$(basename "$src" .c)

    # compile-error fixtures are not run tests
    [ -f "$HERE/$name.experror" ] && continue

    case " $EXCLUDE " in
        *" $name "*)
            printf 'SKIP  %s (feature not in JIT)\n' "$name"
            skip=$((skip + 1))
            continue ;;
    esac

    exitcode_file="$HERE/$name.exitcode"
    [ -f "$exitcode_file" ] || continue
    expect_rc=$(cat "$exitcode_file")

    set +e
    "$JIT" "$src" >/dev/null 2>"$ROOT/build/$name.jiterr"
    rc=$?
    set -e

    if [ "$rc" -eq "$expect_rc" ]; then
        printf 'PASS  %s (exit %d)\n' "$name" "$rc"
        pass=$((pass + 1))
    else
        printf 'FAIL  %s (exit %d, want %d)\n' "$name" "$rc" "$expect_rc"
        sed 's/^/    /' "$ROOT/build/$name.jiterr" 2>/dev/null || true
        fail=$((fail + 1))
    fi
done

printf '\n%d passed, %d failed, %d skipped\n' "$pass" "$fail" "$skip"
[ "$fail" -eq 0 ]
