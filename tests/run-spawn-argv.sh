#!/bin/sh
# run-spawn-argv.sh : the SKJ_SYS_SPAWNV argv round-trip demo.
#
# The parent spawns a child with argv {"child", "hello"} through the
# size-prefixed spawn request; the child echoes argc and each argument. The
# test confirms the arguments crossed the spawn boundary and reached the
# child's main().
#
# Usage: run-spawn-argv.sh <skj-run> <parent.elf> [test-name]

RUN="$1"
PARENT="$2"
NAME="${3:-test-rv-spawn-argv}"
fail=0
tmp=$(mktemp -d)

"$RUN" "$PARENT" >"$tmp/out" 2>"$tmp/err"
rc=$?

grep -q "parent spawned"        "$tmp/out" || { echo "FAIL: parent did not spawn"; fail=1; }
grep -q "child argc=2"          "$tmp/out" || { echo "FAIL: child saw wrong argc"; fail=1; }
grep -q "child argv\[0\]=child" "$tmp/out" || { echo "FAIL: child missing argv[0]"; fail=1; }
grep -q "child argv\[1\]=hello" "$tmp/out" || { echo "FAIL: child missing argv[1]"; fail=1; }
[ "$rc" -eq 0 ] || { echo "FAIL: exit $rc, want 0"; fail=1; }

if [ "$fail" -ne 0 ]; then
    echo "--- stdout ---"; cat "$tmp/out"
    echo "--- stderr ---"; cat "$tmp/err"
    rm -rf "$tmp"
    echo "$NAME: FAILED"
    exit 1
fi
rm -rf "$tmp"
echo "$NAME: argv crossed the spawn boundary and reached the child"
exit 0
