#!/bin/sh
# run-scheme-wasm-tests.sh - compile each tests/scm_*.scm with skj-sc-wasm, run
# the resulting module under wasmtime, and check the exit code against .exitcode
# (default 0; a TinScheme program's top-level value is its exit status).
#
# This is the WASM arm of the TinScheme corpus, the sibling of the native
# per-backend scheme codegen lanes.  Unlike the Pascal and BASIC WASM lanes
# there is no separate link step: skj-sc-wasm emits a complete module with the
# backend's synthesized WASI runtime (the write/read/exit seams over the preview1
# imports and the bump heap).
#
# The lane invokes the exported `main` and checks its returned value (the untagged
# top-level result, the same number the program's exit code would carry) against
# .exitcode (default 0), rather than running the WASI command and reading the
# process exit code.  This is because a program that uses delimited continuations
# (shift/reset, lowered to the WasmFX stack-switching proposal) cannot call
# proc_exit: in the pinned wasmtime 42 any trap, proc_exit included, panics the
# engine once a stack switch has occurred (a benign host call or a normal return
# is fine).  Invoking main sidesteps the WASI exit path for every scheme program
# uniformly.  See doc/internals/wasm-continuations.md.
#
# Environment:
#   SCBIN    - skj-sc-wasm             (default build/skj-sc-wasm)
#   WASMTIME - the wasm runtime        (default wasmtime)
#   OUTDIR   - where .wasm files land  (default build/scheme-wasm)
#   EXCLUDE  - space-separated test names to skip
#
# A missing wasmtime skips the whole suite cleanly (exit 0).

set -eu

HERE=$(dirname "$0")
ROOT=$(cd "$HERE/.." && pwd)
SCBIN="${SCBIN:-$ROOT/build/skj-sc-wasm}"
WASMTIME="${WASMTIME:-wasmtime}"
OUTDIR="${OUTDIR:-$ROOT/build/scheme-wasm}"
EXCLUDE="${EXCLUDE:-}"
mkdir -p "$OUTDIR"

if [ ! -x "$SCBIN" ]; then
    echo "ERROR: $SCBIN not found"
    exit 1
fi
if ! command -v "$WASMTIME" >/dev/null 2>&1; then
    echo "run-scheme-wasm-tests: wasmtime not found, skipping"
    exit 0
fi

fail=0
pass=0
skip=0

for src in "$HERE"/scm_*.scm; do
    [ -f "$src" ] || continue
    name=$(basename "$src" .scm)
    case " $EXCLUDE " in
        *" $name "*)
            printf 'SKIP  %s (excluded)\n' "$name"
            skip=$((skip + 1))
            continue ;;
    esac

    wasm="$OUTDIR/$name.wasm"
    out="$OUTDIR/$name.out"

    set +e
    "$SCBIN" -o "$wasm" "$src" 2>/dev/null
    if [ $? -ne 0 ]; then
        printf 'FAIL  %s (compile error)\n' "$name"
        fail=$((fail + 1)); set -e; continue
    fi
    in=/dev/null
    [ -f "$HERE/$name.input" ] && in="$HERE/$name.input"
    # Invoke main and read its returned value (see the header comment).  The
    # proposal flags enable WasmFX stack switching and its prerequisites; they
    # are inert for a program that does not use continuations.
    got=$("$WASMTIME" run --invoke main \
            -W stack-switching=y -W function-references=y -W gc=y -W exceptions=y \
            "$wasm" <"$in" 2>/dev/null | tr -d '[:space:]')
    set -e

    expect=0
    [ -f "$HERE/$name.exitcode" ] && expect=$(cat "$HERE/$name.exitcode")
    [ -f "$HERE/$name.expect" ] && expect=$(cat "$HERE/$name.expect")
    if [ "$got" != "$expect" ]; then
        printf 'FAIL  %s (main returned %s, want %s)\n' "$name" "${got:-<none>}" "$expect"
        fail=$((fail + 1))
        continue
    fi
    printf 'PASS  %s (main=%s)\n' "$name" "$got"
    pass=$((pass + 1))
done

printf '\n%d passed, %d failed, %d skipped\n' "$pass" "$fail" "$skip"
[ "$fail" -eq 0 ]
