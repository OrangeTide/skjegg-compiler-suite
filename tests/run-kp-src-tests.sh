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
# hosted tiers).
SRCDIR=${KP_SRCDIR:-"$HERE/kp-src"}

MODE=${MODE:-jit}
FRONTEND=${FRONTEND:-"$ROOT/build/skj-kp-jit"}
JIT=${JIT:-"$ROOT/build/skj-kp-jit"}
QEMU=${QEMU:-}

# The build scratch dir is named after the fixture set, the mode, and the mode's
# runner binary, so concurrent make -j targets that share a fixture set do not
# clobber each other's build/<name>.{out,kpm,s,o,elf}.  Without the mode and
# runner tag the jit and aot corpus runs, and the x86-64 and ColdFire aot corpus
# runs, would all land in one build/kp-corpus and race under -j.
case $MODE in
    jit)  _tagbin=$JIT ;;
    aot)  _tagbin=${KPAOT:-$FRONTEND} ;;
    free) _tagbin=${KPRV:-$FRONTEND} ;;
    *)    _tagbin=$FRONTEND ;;
esac
BDIR="$ROOT/build/$(basename "$SRCDIR")-$MODE-$(basename "$_tagbin")"

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

    # Optional expected exit code (default 0).  A trap/fault fixture pins a
    # nonzero code (70 for a Kobold runtime trap) alongside its .expected stdout
    # (empty when the program traps before emitting anything).
    expcode=0
    [ -f "$SRCDIR/$name.exitcode" ] && expcode=$(cat "$SRCDIR/$name.exitcode")

    # A .experror fixture is a compile-fail test: the front end must reject the
    # source, and its stderr must contain every non-blank, non-'#' line of the
    # fixture as a fixed substring (the .experror convention shared with
    # tests/kp and the Excelsior suite).  It is compile-only, so it runs the
    # same way in every mode, using the mode's front end.
    errspec="$SRCDIR/$name.experror"
    if [ -f "$errspec" ]; then
        # the mode's front end (the jit compiler runs under the emulator; the aot
        # and free front ends are native host binaries, so no emulator prefix)
        case "$MODE" in
        jit) comp="$JIT"; pre="$QEMU" ;;
        *)   comp="$FRONTEND"; pre="" ;;
        esac
        set +e
        $pre "$comp" --emit-kpm "$pas" >"$out" 2>"$err"
        got=$?
        set -e
        if [ "$got" -eq 0 ]; then
            ok=0; reason="compiled, expected a compile error"
        else
            while IFS= read -r want; do
                case $want in '' | '#'*) continue ;; esac
                if ! grep -qF -- "$want" "$err"; then
                    ok=0; reason="${reason:+$reason; }missing: $want"
                fi
            done < "$errspec"
        fi
        if [ "$ok" -eq 1 ]; then
            pass=$((pass + 1))
        else
            fail=$((fail + 1))
            printf 'FAIL %s: %s\n' "$name" "$reason"
            [ -s "$err" ] && sed 's/^/    /' "$err"
        fi
        return
    fi

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
        if [ "$got" -ne "$expcode" ]; then
            ok=0; reason="run exit $got, expected $expcode"
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
            if [ "$got" -ne "$expcode" ]; then
                ok=0; reason="aot run exit $got, expected $expcode"
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
            if [ "$got" -ne "$expcode" ]; then
                ok=0; reason="run exit $got, expected $expcode"
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
