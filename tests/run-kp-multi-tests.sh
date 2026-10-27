#!/bin/sh
# run-kp-multi-tests.sh - compile and run multi-unit Kobold-Pascal fixtures.
#
# The single-module runner (run-kp-src-tests.sh) compiles one .pas per fixture.
# A multi-unit program instead `uses` one or more units, each a separate .pas
# that compiles to its own KPM1 module; the program's module carries a `.uses
# NAME` line per dependency, and both the front end (at compile time, to resolve
# the interface) and the loader (the JIT in-memory, or aot-run.sh at link time)
# find NAME.kpm beside the program or under KOBOLD_UNITS.  This runner builds that
# layout: it compiles every unit to a .kpm first, then runs each program against
# the compiled set.
#
# A fixture directory (tests/kp-multi) holds both roles, told apart by the first
# keyword of the file: `unit NAME;` is a dependency (no .expected, compiled to a
# module), `program NAME;` is a test (paired with NAME.expected, its stdout under
# the independent reference kprun).  Sources are staged into the build directory
# so the compiled unit modules land beside them (the front end searches the source
# file's own directory first) and the fixture tree stays pure source.
#
# Two modes, selected by MODE:
#
#   MODE=jit   JIT <prog>.pas compiles the program in-process, loads its unit
#              modules from the build directory, runs it; stdout must equal
#              <prog>.expected.  A QEMU prefix runs a cross JIT.
#
#   MODE=aot   FRONTEND --emit-kpm emits each module, aot-run.sh compiles the
#              program and its reached units and links the whole set, and the
#              program's stdout must equal <prog>.expected.
#
# Environment (same contract as run-kp-src-tests.sh):
#   MODE       jit | aot                                            [default jit]
#   FRONTEND   the --emit-kpm front end (unit precompile, aot) [default skj-kp-jit]
#   JIT        the source JIT runner (jit)                    [default skj-kp-jit]
#   QEMU       an emulator prefix for a cross JIT / aot program          [default none]
#   KPAOT KPAOTLIB KPAOTCC KPAOTAS KPAOTQEMU   passed through to aot-run.sh (aot)

set -eu

HERE=$(dirname "$0")
ROOT=$(cd "$HERE/.." && pwd)
SRCDIR=${KP_SRCDIR:-"$HERE/kp-multi"}

MODE=${MODE:-jit}
FRONTEND=${FRONTEND:-"$ROOT/build/skj-kp-jit"}
JIT=${JIT:-"$ROOT/build/skj-kp-jit"}
QEMU=${QEMU:-}

# Scratch is keyed on the fixture set, the mode, and the runner binary, so that
# concurrent make -j targets sharing a fixture set (the x86-64, arm64, and rv AOT
# runs) do not share a scratch directory.  Their unit .kpm modules now carry the
# target architecture, so a shared directory would let one run's modules clobber
# another's.
case $MODE in
    jit) _tagbin=$JIT ;;
    aot) _tagbin=${KPAOT:-$FRONTEND} ;;
    *)   _tagbin=$FRONTEND ;;
esac
BDIR="$ROOT/build/$(basename "$SRCDIR")-$MODE-$(basename "$_tagbin")"

rm -rf "$BDIR"
mkdir -p "$BDIR"
cp "$SRCDIR"/*.pas "$BDIR/"
# The loader resolves units here too, matching the front end's search.
export KOBOLD_UNITS="$BDIR"

is_unit() { head -1 "$1" | grep -q '^unit '; }

# Compile every unit to a module, in dependency order: a unit that `uses` another
# needs that one's .kpm present first, so iterate to a fixpoint (a unit whose
# dependency is not compiled yet fails and is retried once the dependency lands).
units=
for f in "$BDIR"/*.pas; do
    is_unit "$f" && units="$units $(basename "$f" .pas)"
done
remaining=$units
while [ -n "$(echo "$remaining" | tr -d ' ')" ]; do
    next=
    progress=0
    for u in $remaining; do
        if "$FRONTEND" --emit-kpm "$BDIR/$u.pas" >"$BDIR/$u.kpm" 2>"$BDIR/$u.uerr"; then
            progress=1
        else
            rm -f "$BDIR/$u.kpm"
            next="$next $u"
        fi
    done
    if [ "$progress" -eq 0 ]; then
        echo "run-kp-multi-tests.sh: cannot compile units:$next" >&2
        for u in $next; do sed 's/^/    /' "$BDIR/$u.uerr"; done
        exit 1
    fi
    remaining=$next
done

pass=0
fail=0

run_one() {
    name=$1
    pas="$BDIR/$name.pas"
    exp="$SRCDIR/$name.expected"
    out="$BDIR/$name.out"
    err="$BDIR/$name.err"
    ok=1
    reason=

    # Optional per-fixture stdin (.input) and arguments (.args), as in
    # run-kp-src-tests.sh; $args is intentionally unquoted (whitespace-split).
    stdin_src="$SRCDIR/$name.input"
    [ -f "$stdin_src" ] || stdin_src=/dev/null
    args=""
    [ -f "$SRCDIR/$name.args" ] && args=$(cat "$SRCDIR/$name.args")

    case "$MODE" in
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
    *)
        echo "run-kp-multi-tests.sh: unknown MODE '$MODE'" >&2
        exit 2
        ;;
    esac

    if [ "$ok" -eq 1 ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL %s: %s\n' "$name" "$reason"
        [ -s "$err" ] && sed 's/^/    /' "$err"
        if [ -f "$exp" ] && [ -f "$out" ]; then
            printf '    --- expected ---\n'; sed 's/^/    /' "$exp"
            printf '    --- got ---\n'; sed 's/^/    /' "$out"
        fi
    fi
}

for src in "$SRCDIR"/*.pas; do
    is_unit "$src" && continue
    run_one "$(basename "$src" .pas)"
done

printf 'kp-multi (%s): %d passed, %d failed\n' "$MODE" "$pass" "$fail"
[ "$fail" -eq 0 ]
