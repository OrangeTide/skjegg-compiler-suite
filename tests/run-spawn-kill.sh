#!/bin/sh
# run-spawn-kill.sh : the SKJ_SYS_KILL demo.
#
# The parent spawns a child, kills it with status 9 before it runs, then waits
# and reads that status. The child never runs, so "child running" must be
# absent; the parent must report status 9.
#
# Usage: run-spawn-kill.sh <skj-run> <parent.elf> [test-name]

RUN="$1"
PARENT="$2"
NAME="${3:-test-rv-spawn-kill}"
fail=0
tmp=$(mktemp -d)

"$RUN" "$PARENT" >"$tmp/out" 2>"$tmp/err"
rc=$?

grep -q "parent spawning"       "$tmp/out" || { echo "FAIL: parent did not spawn"; fail=1; }
grep -q "parent killed status=9" "$tmp/out" || { echo "FAIL: wrong kill status"; fail=1; }
if grep -q "child running" "$tmp/out"; then
    echo "FAIL: child ran despite being killed first"
    fail=1
fi
[ "$rc" -eq 0 ] || { echo "FAIL: exit $rc, want 0"; fail=1; }

if [ "$fail" -ne 0 ]; then
    echo "--- stdout ---"; cat "$tmp/out"
    echo "--- stderr ---"; cat "$tmp/err"
    rm -rf "$tmp"
    echo "$NAME: FAILED"
    exit 1
fi
rm -rf "$tmp"
echo "$NAME: parent killed the child and read its status"
exit 0
