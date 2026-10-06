#!/bin/sh
# run-compile-san.sh - compile-only sanitizer probe for a front end.
#
# Each source in a test set is compiled to a scratch .s with a sanitized
# front-end tool (ASan+UBSan). Codegen is where the sanitizers earn their
# keep, so the probe runs the whole front-end pipeline (lex, parse, lower,
# regalloc, emit) and discards the assembly. A nonzero exit is a failure:
# a sanitizer abort (-fno-sanitize-recover) or a front-end error. The guest
# assembly is never assembled or run here; this only instruments the host
# compiler.
#
# Usage: run-compile-san.sh <tool> <label> <source>...
#   TOOL  the sanitized front-end binary (invoked as: TOOL -o out.s src)
# Each remaining argument is a source file to compile.

set -u

TOOL=$1
LABEL=$2
shift 2

HERE=$(dirname "$0")
ROOT=$(cd "$HERE/.." && pwd)
OUT="${OUTDIR:-$ROOT/build/san/compile-out}"
mkdir -p "$OUT"

fail=0
pass=0

for src in "$@"; do
    [ -f "$src" ] || continue
    base=$(basename "$src")
    set +e
    "$TOOL" -o "$OUT/$base.s" "$src" >"$OUT/$base.log" 2>&1
    rc=$?
    set -e 2>/dev/null || true
    if [ "$rc" -ne 0 ]; then
        echo "FAIL ($LABEL): $base (exit $rc)"
        sed 's/^/    /' "$OUT/$base.log"
        fail=$((fail + 1))
    else
        pass=$((pass + 1))
    fi
done

echo "$LABEL: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
