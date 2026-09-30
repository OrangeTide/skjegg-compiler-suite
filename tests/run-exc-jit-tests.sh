#!/bin/sh
# run-exc-jit-tests.sh - the Excelsior end-to-end suite through the in-process
# JIT.  For each tests/exs_*.exs with a .exitcode and/or .expected file, run it
# in-process with skj-exc-jit-arm64 (no assembler, linker, or emulator), compare
# the exit code to the .exitcode file (default 0), and, when a .expected file is
# present, diff its stdout.  .experror tests (compile-error fixtures) are not run
# here; they belong to the front-end suite.
#
#   JIT     - the exc JIT driver (default build/skj-exc-jit-arm64)
#   RUNNER  - a command prefix to run it under (e.g. qemu-aarch64)
#   EXCLUDE - space-separated test names to skip (features not yet in the JIT)

set -eu

HERE=$(dirname "$0")
ROOT=$(cd "$HERE/.." && pwd)
JIT="${JIT:-$ROOT/build/skj-exc-jit-arm64}"
RUNNER="${RUNNER:-}"
EXCLUDE="${EXCLUDE:-}"

pass=0
fail=0
skip=0

# runnable tests: those with a .exitcode or .expected (not .experror)
names=$(ls "$HERE"/exs_*.exitcode "$HERE"/exs_*.expected 2>/dev/null |
        sed -e 's#.*/##' -e 's/\.exitcode$//' -e 's/\.expected$//' |
        sort -u)

for name in $names; do
    src="$HERE/$name.exs"
    [ -f "$src" ] || continue
    case " $EXCLUDE " in
        *" $name "*) skip=$((skip + 1)); continue ;;
    esac

    expect=0
    [ -f "$HERE/$name.exitcode" ] && expect=$(cat "$HERE/$name.exitcode")
    expfile="$HERE/$name.expected"
    out="$ROOT/build/$name.jitout"

    set +e
    $RUNNER "$JIT" "$src" >"$out" 2>/dev/null
    got=$?
    set -e

    ok=1
    reason=
    if [ "$got" -ne "$expect" ]; then
        ok=0
        reason="exit $got, expected $expect"
    fi
    if [ -f "$expfile" ] && ! diff -u "$expfile" "$out" >"$out.diff" 2>&1; then
        ok=0
        reason="${reason:+$reason; }stdout differs"
    fi

    if [ "$ok" -eq 1 ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL %s: %s\n' "$name" "$reason"
        [ -f "$expfile" ] && [ -s "$out.diff" ] && sed 's/^/    /' "$out.diff"
    fi
done

printf '\n%d passed, %d failed, %d skipped\n' "$pass" "$fail" "$skip"
[ "$fail" -eq 0 ]
