#!/bin/sh
# run-xlang-sandbox.sh : the cross-language RV32 process-sandbox demo.
#
# One root process, built from C by skj-cc-rv, spawns two children: one built
# from C (cchild.c) and one from Kobold-Pascal (kchild.pas, through
# skj-kpc -> skj-kp-rv -> skj-as-rv -> skj-ld-rv over the freestanding crt).
# skj-run round-robin schedules all three under one shared memory budget. The
# children are RV32 ELFs the toolkit produced from the shared IR, so the
# sandbox treats a C process and a Pascal process identically; this is the
# cross-language counterpart of test-rv-sandbox.
#
# Output interleaves under the scheduler, so each marker is checked for
# presence, not order (as test-rv-sandbox does). Both children run the same
# computation (sum 1..99 = 4950) in their two source languages, so the matching
# markers make the point directly.
#
# Usage: run-xlang-sandbox.sh <skj-run> <parent.elf>

RUN="$1"
PARENT="$2"
fail=0
tmp=$(mktemp -d)

"$RUN" --total-mem 256M "$PARENT" >"$tmp/out" 2>"$tmp/err"
rc=$?

check() {
    grep -qF "$1" "$tmp/out" || { echo "FAIL: missing marker: $1"; fail=1; }
}

# the C root
check "parent[C]: running"
check "parent[C]: spawned C child"
check "parent[C]: spawned Kobold child"
check "parent[C]: done"
# the C child
check "child[C]: running"
check "child[C]: sum=4950"
check "child[C]: done"
# the Kobold child: the same sum, plus a second computed line
check "child[kobold]: running"
check "child[kobold]: sum=4950"
check "child[kobold]: 6*7=42"
check "child[kobold]: done"

[ "$rc" -eq 0 ] || { echo "FAIL: root exit $rc, want 0"; fail=1; }

if [ "$fail" -ne 0 ]; then
    echo "--- stdout ---"; cat "$tmp/out"
    echo "--- stderr ---"; cat "$tmp/err"
    rm -rf "$tmp"
    echo "test-rv-xlang-sandbox: FAILED"
    exit 1
fi

rm -rf "$tmp"
echo "test-rv-xlang-sandbox: a C root, a C child, and a Kobold child shared one sandbox"
exit 0
