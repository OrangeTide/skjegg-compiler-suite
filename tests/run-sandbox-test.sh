#!/bin/sh
# run-sandbox-test.sh : the RV32 process-sandbox demo.
#
# The parent spawns a child from an embedded image and does a guarded map/unmap;
# the child maps a large region and touches every page. Two runs check the
# scheduler, spawn, and the aggregate memory budget:
#
#   generous --total-mem : the child finishes ("child done")
#   tight --total-mem    : the child exhausts the shared pool and faults, while
#                          the parent, isolated from it, still finishes and the
#                          run exits cleanly
#
# Usage: run-sandbox-test.sh <skj-run> <parent.elf>

RUN="$1"
PARENT="$2"
fail=0
tmp=$(mktemp -d)

# Run A: a generous budget. Parent spawns the child; both finish.
"$RUN" --total-mem 64M "$PARENT" >"$tmp/outA" 2>"$tmp/errA"
rcA=$?
grep -q "parent done" "$tmp/outA" || { echo "FAIL: run A missing 'parent done'"; fail=1; }
grep -q "child done"  "$tmp/outA" || { echo "FAIL: run A missing 'child done'"; fail=1; }
[ "$rcA" -eq 0 ] || { echo "FAIL: run A exit $rcA, want 0"; fail=1; }

# Run B: a tight budget. The child exhausts the shared pool and faults; the
# parent, isolated, still finishes and the run exits cleanly.
"$RUN" --total-mem 8M "$PARENT" >"$tmp/outB" 2>"$tmp/errB"
rcB=$?
grep -q "memory pool exhausted" "$tmp/errB" || { echo "FAIL: run B did not report pool exhaustion"; fail=1; }
grep -q "parent done"  "$tmp/outB" || { echo "FAIL: run B parent did not survive"; fail=1; }
grep -q "child running" "$tmp/outB" || { echo "FAIL: run B child did not start"; fail=1; }
if grep -q "child done" "$tmp/outB"; then
    echo "FAIL: run B child should not finish under the tight budget"
    fail=1
fi
[ "$rcB" -eq 0 ] || { echo "FAIL: run B exit $rcB, want 0 (the root's status)"; fail=1; }

rm -rf "$tmp"
if [ "$fail" -ne 0 ]; then
    echo "test-rv-sandbox: FAILED"
    exit 1
fi
echo "test-rv-sandbox: scheduler, spawn, and the shared memory budget all passed"
exit 0
