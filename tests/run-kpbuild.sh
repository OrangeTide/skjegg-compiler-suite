#!/bin/sh
# The module-builder gate.  For each source module, emit a .kpm with the front
# end, then confirm the builder self-test rebuilds it (read -> replay through the
# kpb_* entry points) with assembly byte-identical to the module read straight
# from the file.
#
#   KPC=build/skj-kpc SELF=build/skj-kpbuild-selftest sh tests/run-kpbuild.sh file.pas ...
set -e

KPC=${KPC:-build/skj-kpc}
SELF=${SELF:-build/skj-kpbuild-selftest}
TARGET=${TARGET:-KP_TARGET_X64}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fail=0
pass=0
for src in "$@"; do
    base=$(basename "$src" | sed 's/\.[^.]*$//')
    kpm="$WORK/$base.kpm"
    if ! "$KPC" -D "$TARGET" --emit-kpm "$src" > "$kpm" 2>"$WORK/$base.err"; then
        echo "FAIL $base: --emit-kpm failed"
        cat "$WORK/$base.err"
        fail=$((fail + 1))
        continue
    fi
    if "$SELF" "$kpm"; then
        pass=$((pass + 1))
    else
        echo "FAIL $base: builder asm mismatch"
        fail=$((fail + 1))
    fi
done

echo "kpbuild: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
