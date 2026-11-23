#!/bin/sh
# run-selfhost-wasm-tests.sh - prove the TinScheme compiler is self-hosting on
# WASM.  For each tests/scm_*.scm, compile it two ways and require the results to
# match byte for byte:
#   1. the NATIVE cross compiler skj-sc-wasm (a host binary emitting WASM), and
#   2. the SELF-HOSTED skj-sc.wasm (skjegg's own tools compiled the compiler to a
#      WASM module), run under wasmtime with a preopened working directory.
# A byte-identical module is the self-host proof: check-sc-wasm already run-checks
# the native compiler's output, so an identical module behaves identically.
#
# Env: SC_SELFHOST (the self-hosted skj-sc.wasm), SC_NATIVE (native skj-sc-wasm),
#      WASMTIME, OUTDIR.  Skips cleanly when wasmtime is absent.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
: "${SC_SELFHOST:?}"; : "${SC_NATIVE:?}"; : "${OUTDIR:?}"
WASMTIME="${WASMTIME:-wasmtime}"

if ! command -v "$WASMTIME" >/dev/null 2>&1; then
    echo "check-selfhost-wasm: wasmtime not found, skipping"
    exit 0
fi

rm -rf "$OUTDIR"
mkdir -p "$OUTDIR"

pass=0; fail=0

for src in "$HERE"/scm_*.scm; do
    [ -f "$src" ] || continue
    name=$(basename "$src" .scm)

    # native compile (host binary -> WASM module)
    if ! "$SC_NATIVE" -o "$OUTDIR/$name.ref.wasm" "$src" 2>"$OUTDIR/$name.ref.err"; then
        printf 'FAIL  %s (native compile)\n' "$name"
        fail=$((fail + 1)); continue
    fi

    # self-hosted compile: copy the source into the preopened dir and run the
    # compiler WASM under wasmtime with relative paths
    cp "$src" "$OUTDIR/$name.scm"
    set +e
    ( cd "$OUTDIR" && "$WASMTIME" run --dir=.::. "$SC_SELFHOST" \
        -o "$name.self.wasm" "$name.scm" ) 2>"$OUTDIR/$name.self.err"
    rc=$?
    set -e
    if [ "$rc" -ne 0 ]; then
        printf 'FAIL  %s (self-hosted compile exit %s): %s\n' \
            "$name" "$rc" "$(head -1 "$OUTDIR/$name.self.err")"
        fail=$((fail + 1)); continue
    fi

    if cmp -s "$OUTDIR/$name.ref.wasm" "$OUTDIR/$name.self.wasm"; then
        printf 'PASS  %s (%s bytes, byte-identical)\n' \
            "$name" "$(wc -c <"$OUTDIR/$name.ref.wasm" | tr -d ' ')"
        pass=$((pass + 1))
    else
        printf 'FAIL  %s (self-hosted output differs from native)\n' "$name"
        fail=$((fail + 1))
    fi
done

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
