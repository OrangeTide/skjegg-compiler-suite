#!/bin/sh
# Acceptance test: the Kobold compiler assembled two independent ways must emit
# the same module.  Both sides are single-step (they drive the C module builder
# and serialize with kpm_write; neither has a text emitter), but they are built
# from genuinely different pipelines, so a byte-for-byte match proves the two
# agree rather than being the same binary twice.
#
#   KPC  the native-linked compiler: kpcom compiled ahead-of-time by the skj-pc
#        Pascal front end (build/skj-kpc-rv), linked directly to the C builder.
#        A host binary that emits rv32 KPM.
#   REF  the JIT'd-seed compiler: kpcom JIT'd from its embedded rv32 seed
#        (build/skj-kp-jit-rv), driving the same C builder through its bound
#        kpbp_* imports.  A cross binary, so it runs under QEMU.
#
# Both target rv32, so their KPM is byte-comparable.  The x86-64 skj-kp-jit links
# the same kpcom object as skj-kpc and would only compare a build to itself; the
# rv pair is the real native-link vs JIT'd-seed check.
#
# Env:
#   KPC     the skj-pc-built native-linked compiler  (required)
#   REF     the JIT'd-seed compiler, run as `[QEMU] REF --emit-kpm f` (required)
#   QEMU    emulator prefix for REF (e.g. qemu-riscv32); empty to run REF natively
#   CORPUS  directory of .pas modules to compare      (required)
#   OUT     a writable directory for build artifacts  (required)

set -u
KPC=${KPC:?KPC not set}
REF=${REF:?REF not set}
QEMU=${QEMU:-}
CORPUS=${CORPUS:?CORPUS not set}
OUT=${OUT:?OUT not set}

ours="$OUT/pc-kpcom.ours.kpm"
ref="$OUT/pc-kpcom.ref.kpm"
same=0
refskip=0
fail=0
failed=""
for f in "$CORPUS"/*.pas; do
    b=$(basename "$f" .pas)
    # Modules the reference itself cannot emit are not ours to match; skip them
    # so this gate tracks only agreement, not the reference's own coverage.
    if ! $QEMU "$REF" --emit-kpm "$f" >"$ref" 2>/dev/null; then
        refskip=$((refskip+1)); continue
    fi
    "$KPC" --emit-kpm "$f" >"$ours" 2>/dev/null
    rc=$?
    if [ "$rc" -ge 132 ]; then
        failed="$failed $b(crash:$rc)"; fail=$((fail+1)); continue
    fi
    if cmp -s "$ref" "$ours"; then
        same=$((same+1))
    else
        failed="$failed $b(differ)"; fail=$((fail+1))
    fi
done

echo "pc-kpcom: corpus KPM match same=$same fail=$fail ref-skipped=$refskip"
if [ "$fail" -ne 0 ]; then
    echo "FAILED:$failed" >&2
    exit 1
fi
