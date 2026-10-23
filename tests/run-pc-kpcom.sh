#!/bin/sh
# Acceptance test: compile the real Kobold compiler, kobold/kpcom.pas, with the
# skj-pc Pascal front end (KOBOLD defined, so its file I/O is external cdecl),
# link it against the host runtime and the host-import shims, then check that
# the resulting kpcom emits byte-identical KPM modules to the reference front
# end (build/skj-kp-jit) for every module in the behavioral corpus.
#
# The kpcom build is the long pole here (~1 minute), so its wall time is printed
# as a "pc-kpcom: build ..." metric line for CI and development tracking, with a
# per-stage breakdown (front end, assembler, link).
#
# Env:
#   PC      the skj-pc x86-64 binary                 (required)
#   RT      the host Pascal runtime object           (required)
#   HOST    the host-import shim C source            (required)
#   SRC     kpcom.pas                                (required)
#   REF     the reference emitter, run as `REF --emit-kpm f` (required)
#   CORPUS  directory of .pas modules to compare     (required)
#   OUT     a writable directory for build artifacts (required)
#   NASM    the NASM assembler                        (default: nasm)
#   CC      the host C linker driver                  (default: cc)

set -u
PC=${PC:?PC not set}
RT=${RT:?RT not set}
HOST=${HOST:?HOST not set}
SRC=${SRC:?SRC not set}
REF=${REF:?REF not set}
CORPUS=${CORPUS:?CORPUS not set}
OUT=${OUT:?OUT not set}
NASM=${NASM:-nasm}
CC=${CC:-cc}

asm="$OUT/pc-kpcom.asm"
obj="$OUT/pc-kpcom.o"
bin="$OUT/pc-kpcom"
elapsed() { awk -v a="$1" -v b="$2" 'BEGIN { printf "%.2f", b - a }'; }

# ---- build kpcom with the skj-pc front end, timing each stage ----
t0=$(date +%s.%N)
if ! "$PC" -d KOBOLD -o "$asm" "$SRC" 2>"$OUT/pc-kpcom.feerr"; then
    echo "pc-kpcom: FAIL front end did not compile $SRC" >&2
    sed 's/^/  /' "$OUT/pc-kpcom.feerr" >&2
    exit 1
fi
t1=$(date +%s.%N)
if ! "$NASM" -f elf64 "$asm" -o "$obj" 2>"$OUT/pc-kpcom.aserr"; then
    echo "pc-kpcom: FAIL assembler rejected the emitted module" >&2
    exit 1
fi
t2=$(date +%s.%N)
if ! "$CC" -no-pie "$obj" "$HOST" "$RT" -lm -o "$bin" 2>"$OUT/pc-kpcom.lderr"; then
    echo "pc-kpcom: FAIL link" >&2
    sed 's/^/  /' "$OUT/pc-kpcom.lderr" >&2
    exit 1
fi
t3=$(date +%s.%N)

echo "pc-kpcom: build $(elapsed "$t0" "$t3")s" \
     "(front end $(elapsed "$t0" "$t1")s," \
     "assemble $(elapsed "$t1" "$t2")s," \
     "link $(elapsed "$t2" "$t3")s)"

# ---- compare emitted modules to the reference, byte for byte ----
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
    if ! "$REF" --emit-kpm "$f" >"$ref" 2>/dev/null; then
        refskip=$((refskip+1)); continue
    fi
    "$bin" --emit-kpm "$f" >"$ours" 2>/dev/null
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
