#!/bin/sh
# run-spawn-wait.sh : the SKJ_SYS_WAIT demo.
#
# The parent spawns a child and blocks in wait() until it exits, then reads back
# the child's exit status (7). The parent reaches wait() before the child runs,
# so this also exercises the park-and-wake path and the zombie-then-reap flow.
#
# Usage: run-spawn-wait.sh <skj-run> <parent.elf> [test-name]

RUN="$1"
PARENT="$2"
NAME="${3:-test-rv-spawn-wait}"
fail=0
tmp=$(mktemp -d)

"$RUN" "$PARENT" >"$tmp/out" 2>"$tmp/err"
rc=$?

grep -q "parent spawning"        "$tmp/out" || { echo "FAIL: parent did not spawn"; fail=1; }
grep -q "child running"          "$tmp/out" || { echo "FAIL: child did not run"; fail=1; }
grep -q "parent child status=7"  "$tmp/out" || { echo "FAIL: parent read the wrong status"; fail=1; }
[ "$rc" -eq 0 ] || { echo "FAIL: exit $rc, want 0"; fail=1; }

if [ "$fail" -ne 0 ]; then
    echo "--- stdout ---"; cat "$tmp/out"
    echo "--- stderr ---"; cat "$tmp/err"
    rm -rf "$tmp"
    echo "$NAME: FAILED"
    exit 1
fi
rm -rf "$tmp"
echo "$NAME: parent waited on the child and read its exit status"
exit 0
