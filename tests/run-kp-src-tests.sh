#!/bin/sh
# run-kp-src-tests.sh - compile and run Kobold-Pascal source fixtures end to end.
#
# The fixtures live in tests/kp-src/<name>.pas, each paired with a
# tests/kp-src/<name>.expected file holding the program's expected stdout.  This
# is the source-compilation counterpart of run-kp-tests.sh (which exercises the
# KPM1 reader on pre-built modules): here the input is Kobold source, compiled
# by the embedded or native front end.
#
# A fixture may also carry input: a <name>.input file is fed to the program as
# stdin, and a <name>.args file (one line, whitespace-separated) is passed as its
# command-line arguments (ParamStr/ParamCount).  The .expected is the reference
# kprun's output under the same stdin and arguments.  These apply in the run modes
# (jit, aot, free); the frontend mode does not run the program.
#
# One script serves three modes, selected by MODE:
#
#   MODE=frontend  host smoke of the in-process front end.  FRONTEND --emit-kpm
#                  <name>.pas must succeed and its stdout must begin with the
#                  KPM1 header '.kpm 1'.  No run, no .expected compare; it proves
#                  kp_compile_pas turns source into a module.
#
#   MODE=jit       compile and run in one process.  JIT <name>.pas must run and
#                  its stdout must equal <name>.expected.  A QEMU prefix runs a
#                  cross JIT under an emulator.
#
#   MODE=aot       the native front end emits a module (FRONTEND --emit-kpm), the
#                  backend AOT-compiles and links it (kobold/aot-run.sh over the
#                  KPAOT* environment), and the resulting program's stdout must
#                  equal <name>.expected.
#
#   MODE=free      the freestanding boris pipeline: the native front end emits a
#                  module (FRONTEND --emit-kpm), KPRV compiles it to rv32 asm, KPAS
#                  assembles it, and KPLD links the crt (KPCRT), the program, and
#                  the freestanding archive (KPLIB) into a static rv32 ELF whose
#                  only host needs are write/read/exit and the handed-in heap.  Run
#                  under QEMU; stdout must equal <name>.expected.
#
# Environment:
#   MODE       frontend | jit | aot | free                           [default jit]
#   FRONTEND   the --emit-kpm front end (frontend, aot, free) [default build/skj-kp-jit]
#   JIT        the source JIT runner (jit)                   [default build/skj-kp-jit]
#   QEMU       an emulator prefix for a cross JIT / the free ELF         [default none]
#   KPAOT KPAOTLIB KPAOTCC KPAOTAS KPAOTQEMU   passed through to aot-run.sh (aot)
#   KPRV KPAS KPLD KPCRT KPLIB                 the freestanding toolchain (free)

set -eu

HERE=$(dirname "$0")
ROOT=$(cd "$HERE/.." && pwd)
# KP_SRCDIR selects the fixture directory (default tests/kp-src, the portable set
# every mode runs; tests/kp-corpus is the broader behavioral corpus run on the
# hosted tiers).  The build scratch dir is named after it so the two never clash.
SRCDIR=${KP_SRCDIR:-"$HERE/kp-src"}
BDIR="$ROOT/build/$(basename "$SRCDIR")"

MODE=${MODE:-jit}
FRONTEND=${FRONTEND:-"$ROOT/build/skj-kp-jit"}
JIT=${JIT:-"$ROOT/build/skj-kp-jit"}
QEMU=${QEMU:-}

mkdir -p "$BDIR"

pass=0
fail=0

run_one() {
    name=$1
    pas="$SRCDIR/$name.pas"
    exp="$SRCDIR/$name.expected"
    out="$BDIR/$name.out"
    err="$BDIR/$name.err"
    ok=1
    reason=

    # Optional per-fixture input: a .input file is the program's stdin, a .args
    # file one line of whitespace-separated arguments (ParamStr/ParamCount).  The
    # reference kprun's .expected was produced with the same stdin and arguments.
    # $args is intentionally unquoted below, so the arguments are whitespace-split;
    # a fixture argument may not itself contain whitespace.
    stdin_src="$SRCDIR/$name.input"
    [ -f "$stdin_src" ] || stdin_src=/dev/null
    args=""
    [ -f "$SRCDIR/$name.args" ] && args=$(cat "$SRCDIR/$name.args")

    case "$MODE" in
    frontend)
        set +e
        "$FRONTEND" --emit-kpm "$pas" >"$out" 2>"$err"
        got=$?
        set -e
        if [ "$got" -ne 0 ]; then
            ok=0; reason="front end exit $got"
        elif [ "$(head -1 "$out")" != ".kpm 1" ]; then
            ok=0; reason="output does not begin with '.kpm 1'"
        fi
        ;;
    jit)
        set +e
        $QEMU "$JIT" "$pas" $args >"$out" 2>"$err" <"$stdin_src"
        got=$?
        set -e
        if [ "$got" -ne 0 ]; then
            ok=0; reason="run exit $got"
        elif ! cmp -s "$out" "$exp"; then
            ok=0; reason="stdout differs from $name.expected"
        fi
        ;;
    aot)
        set +e
        "$FRONTEND" --emit-kpm "$pas" >"$BDIR/$name.kpm" 2>"$err"
        got=$?
        set -e
        if [ "$got" -ne 0 ]; then
            ok=0; reason="front end exit $got"
        else
            set +e
            sh "$ROOT/kobold/aot-run.sh" "$BDIR/$name.kpm" $args >"$out" 2>>"$err" <"$stdin_src"
            got=$?
            set -e
            if [ "$got" -ne 0 ]; then
                ok=0; reason="aot run exit $got"
            elif ! cmp -s "$out" "$exp"; then
                ok=0; reason="stdout differs from $name.expected"
            fi
        fi
        ;;
    free)
        set +e
        "$FRONTEND" --emit-kpm "$pas" >"$BDIR/$name.kpm" 2>"$err"
        got=$?
        [ "$got" -eq 0 ] && { "$KPRV" "$BDIR/$name.kpm" -o "$BDIR/$name.s" 2>>"$err"; got=$?; }
        [ "$got" -eq 0 ] && { $KPAS -o "$BDIR/$name.o" "$BDIR/$name.s" 2>>"$err"; got=$?; }
        [ "$got" -eq 0 ] && { $KPLD -o "$BDIR/$name.elf" "$KPCRT" "$BDIR/$name.o" "$KPLIB" 2>>"$err"; got=$?; }
        set -e
        if [ "$got" -ne 0 ]; then
            ok=0; reason="build failed (stage exit $got)"
        else
            set +e
            $QEMU "$BDIR/$name.elf" $args >"$out" 2>>"$err" <"$stdin_src"
            got=$?
            set -e
            if [ "$got" -ne 0 ]; then
                ok=0; reason="run exit $got"
            elif ! cmp -s "$out" "$exp"; then
                ok=0; reason="stdout differs from $name.expected"
            fi
        fi
        ;;
    *)
        echo "run-kp-src-tests.sh: unknown MODE '$MODE'" >&2
        exit 2
        ;;
    esac

    if [ "$ok" -eq 1 ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL %s: %s\n' "$name" "$reason"
        [ -s "$err" ] && sed 's/^/    /' "$err"
        if [ "$MODE" != frontend ] && [ -f "$exp" ] && [ -f "$out" ]; then
            printf '    --- expected ---\n'; sed 's/^/    /' "$exp"
            printf '    --- got ---\n'; sed 's/^/    /' "$out"
        fi
    fi
}

for src in "$SRCDIR"/*.pas; do
    run_one "$(basename "$src" .pas)"
done

printf 'kp-src (%s): %d passed, %d failed\n' "$MODE" "$pass" "$fail"
[ "$fail" -eq 0 ]
