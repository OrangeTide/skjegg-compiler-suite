#!/bin/sh
# test-valgrind.sh - verify no memory leaks on error and normal paths
#
# Runs each compiler/tool under valgrind with both valid and
# deliberately broken inputs.  Checks that:
#   1. the exit code matches expectations (0 for success, 1 for errors)
#   2. valgrind reports zero leaked bytes
#
# Usage: sh tests/test-valgrind.sh [builddir]

set -eu

BUILDDIR=${1:-build}
PASS=0
FAIL=0

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

check() {
    name=$1
    expect_rc=$2
    shift 2

    set +e
    valgrind --leak-check=full --errors-for-leak-kinds=definite \
        --error-exitcode=99 "$@" >"$TMPDIR/out" 2>"$TMPDIR/err"
    rc=$?
    set -e

    if [ "$rc" -eq 99 ]; then
        printf 'FAIL  %-40s (valgrind error)\n' "$name"
        grep -E '(LEAK SUMMARY|definitely|Invalid)' "$TMPDIR/err" | head -5
        FAIL=$((FAIL + 1))
    elif [ "$rc" -ne "$expect_rc" ]; then
        printf 'FAIL  %-40s (rc=%d, want %d)\n' "$name" "$rc" "$expect_rc"
        FAIL=$((FAIL + 1))
    else
        printf 'PASS  %-40s\n' "$name"
        PASS=$((PASS + 1))
    fi
}

# -- create broken input files -----------------------------------------

cat >"$TMPDIR/bad.tc" <<'EOF'
@@@
EOF

cat >"$TMPDIR/bad.pas" <<'EOF'
@@@
EOF

cat >"$TMPDIR/bad.scm" <<'EOF'
(define (foo) (+ 1
EOF

cat >"$TMPDIR/bad.moo" <<'EOF'
@@@
EOF

cat >"$TMPDIR/bad.c" <<'EOF'
int f() { return @@@; }
EOF

cat >"$TMPDIR/bad.s" <<'EOF'
.text
.globl _start
_start:
    blergh
EOF

echo 'not an elf file' >"$TMPDIR/bad.o"

cat >"$TMPDIR/cpp_error.c" <<'EOF'
#error "deliberate"
EOF

# valid inputs that exercise deeper pipeline stages
cat >"$TMPDIR/ok.tc" <<'EOF'
fn main() -> int {
    return 42;
}
EOF

cat >"$TMPDIR/ok.pas" <<'EOF'
program ok;
begin
    halt(42);
end.
EOF

cat >"$TMPDIR/ok.scm" <<'EOF'
(define (main) 42)
EOF

cat >"$TMPDIR/ok.moo" <<'EOF'
verb main
    return 42
end
EOF

cat >"$TMPDIR/ok.c" <<'EOF'
int main(void) { return 42; }
EOF

cat >"$TMPDIR/ok.h" <<'EOF'
#define ANSWER 42
EOF

cat >"$TMPDIR/ok_cpp.c" <<'EOF'
#include "ok.h"
int x = ANSWER;
EOF

# -- skj-tinc ----------------------------------------------------------

check "tinc: happy path"       0  ./$BUILDDIR/skj-tinc -o /dev/null tests/hello.tc
check "tinc: missing file"     1  ./$BUILDDIR/skj-tinc -o /dev/null "$TMPDIR/nonexistent.tc"
check "tinc: syntax error"     1  ./$BUILDDIR/skj-tinc -o /dev/null "$TMPDIR/bad.tc"
check "tinc: bad output path"  1  ./$BUILDDIR/skj-tinc -o /proc/nonexistent/out "$TMPDIR/ok.tc"

# -- skj-pc (pascal) ---------------------------------------------------

check "pascal: happy path"     0  ./$BUILDDIR/skj-pc -o /dev/null tests/pascal_t001_empty.pas
check "pascal: missing file"   1  ./$BUILDDIR/skj-pc -o /dev/null "$TMPDIR/nonexistent.pas"
check "pascal: syntax error"   1  ./$BUILDDIR/skj-pc -o /dev/null "$TMPDIR/bad.pas"
check "pascal: bad output"     1  ./$BUILDDIR/skj-pc -o /proc/nonexistent/out "$TMPDIR/ok.pas"

# -- skj-sc (scheme) ---------------------------------------------------

check "scheme: happy path"     0  ./$BUILDDIR/skj-sc -o /dev/null tests/scm_fact.scm
check "scheme: missing file"   1  ./$BUILDDIR/skj-sc -o /dev/null "$TMPDIR/nonexistent.scm"
check "scheme: syntax error"   1  ./$BUILDDIR/skj-sc -o /dev/null "$TMPDIR/bad.scm"

# -- skj-mooc (moo) ----------------------------------------------------

check "moo: happy path"        0  ./$BUILDDIR/skj-mooc -o /dev/null tests/moo_arith.moo
check "moo: missing file"      1  ./$BUILDDIR/skj-mooc -o /dev/null "$TMPDIR/nonexistent.moo"
check "moo: syntax error"      1  ./$BUILDDIR/skj-mooc -o /dev/null "$TMPDIR/bad.moo"
check "moo: bad output"        1  ./$BUILDDIR/skj-mooc -o /proc/nonexistent/out "$TMPDIR/ok.moo"

# -- skj-cpp ------------------------------------------------------------

check "cpp: happy path"        0  ./$BUILDDIR/skj-cpp -I"$TMPDIR" "$TMPDIR/ok_cpp.c"
check "cpp: missing file"      1  ./$BUILDDIR/skj-cpp "$TMPDIR/nonexistent.c"
check "cpp: #error"            1  ./$BUILDDIR/skj-cpp "$TMPDIR/cpp_error.c"
check "cpp: bad output"        1  ./$BUILDDIR/skj-cpp -o /proc/nonexistent/out "$TMPDIR/ok_cpp.c"

# -- skj-cc -------------------------------------------------------------

check "cc: happy path"         0  ./$BUILDDIR/skj-cc -o /dev/null "$TMPDIR/ok.c"
check "cc: missing file"       1  ./$BUILDDIR/skj-cc "$TMPDIR/nonexistent.c"
check "cc: syntax error"       1  ./$BUILDDIR/skj-cc -o /dev/null "$TMPDIR/bad.c"

# -- skj-as -------------------------------------------------------------

# generate valid assembly from a known-good source for the happy path
./$BUILDDIR/skj-tinc -o "$TMPDIR/ok.s" tests/hello.tc

check "as: happy path"         0  ./$BUILDDIR/skj-as -o /dev/null "$TMPDIR/ok.s"
check "as: missing file"       1  ./$BUILDDIR/skj-as "$TMPDIR/nonexistent.s"
check "as: bad assembly"       1  ./$BUILDDIR/skj-as -o /dev/null "$TMPDIR/bad.s"

# -- skj-ld -------------------------------------------------------------

# assemble a valid object for the happy-path link test
./$BUILDDIR/skj-as -o "$TMPDIR/start.o" runtime/start.S
./$BUILDDIR/skj-as -o "$TMPDIR/ok.o" "$TMPDIR/ok.s"

check "ld: happy path"         0  ./$BUILDDIR/skj-ld -o /dev/null "$TMPDIR/start.o" "$TMPDIR/ok.o"
check "ld: missing file"       1  ./$BUILDDIR/skj-ld -o /dev/null "$TMPDIR/nonexistent.o"
check "ld: bad object"         1  ./$BUILDDIR/skj-ld -o /dev/null "$TMPDIR/bad.o"

# -- summary ------------------------------------------------------------

printf '\n%d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
