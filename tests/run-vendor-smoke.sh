#!/bin/sh
# run-vendor-smoke.sh : vendor the local repo and build a working sandbox tree.
#
# Nothing else builds a vendored tree, so vendor.sh staging/wiring gaps (a file
# not copied, an -I path not set, a stale source list) stay invisible until a
# consumer hits them.  This vendors riscv + cc + toolchain + runtime + sandbox +
# emulator from the local checkout, builds the whole thing with the generated
# skjegg.mk, then compiles a guest with the vendored driver and runs it confined
# under the vendored skj-run: a granted mount reads back (exit 42), an escape
# above the mount is denied (exit 7), and with no grant a bare path lands on the
# empty scratch ramfs (exit 10).  Any missing piece fails the build or a case.
#
# Usage: run-vendor-smoke.sh <repo-dir>
# Needs the RISC-V cross assembler/linker/gcc (for the crt and soft64 helper),
# the same tools the rv build job already has.

set -e

REPO=$1
[ -n "$REPO" ] || { echo "usage: $0 <repo-dir>" >&2; exit 2; }
REPO=$(cd "$REPO" && pwd)

# vendor.sh clones ORIGIN at a branch/tag ref.  Vendor the commit under test:
# use the current branch, or a throwaway branch at HEAD when detached (CI).
ref=$(git -C "$REPO" symbolic-ref --quiet --short HEAD || true)
tmpbranch=""
if [ -z "$ref" ]; then
    tmpbranch="__vendor_smoke_$$"
    git -C "$REPO" branch -f "$tmpbranch" HEAD
    ref="$tmpbranch"
fi

work=$(mktemp -d)
cleanup() {
    rm -rf "$work"
    [ -n "$tmpbranch" ] && git -C "$REPO" branch -D "$tmpbranch" >/dev/null 2>&1 || true
}
trap cleanup EXIT

# Point a copy of vendor.sh at the local repo (ORIGIN is hardcoded in it).
sed "s|^ORIGIN=.*|ORIGIN=\"$REPO/.git\"|" "$REPO/vendor.sh" > "$work/vendor.sh"

echo "== vendoring riscv cc toolchain,runtime,sandbox,emulator (ref $ref) =="
sh "$work/vendor.sh" -d "$work/skjegg" -u "$work" -r "$ref" \
   --arch riscv --frontend cc --with toolchain,runtime,sandbox,emulator >/dev/null

cd "$work"
cat > Makefile <<'MK'
include skjegg/skjegg.mk
build:
	mkdir -p build
all: $(SKJ_ALL) build/lib/riscv/libc.a
MK

echo "== building the vendored tree =="
make all >build.log 2>&1 || { echo "FAIL: vendored build"; cat build.log; exit 1; }

mkdir -p data
printf '*' > data/hello.txt        # byte 0x2a = 42
printf 'X' > secret.txt            # just outside the mount
cat > confguest.c <<'EOF'
#include <fcntl.h>
#include <unistd.h>
int main(void)
{
    char b[16];
    int fd = open("data:hello.txt", 0, 0);      /* O_RDONLY */
    if (fd < 0) return 10;
    int n = read(fd, b, sizeof b);
    close(fd);
    if (n <= 0) return 11;
    return (unsigned char)b[0];                  /* 42 */
}
EOF
cat > escguest.c <<'EOF'
#include <fcntl.h>
int main(void) { int fd = open("data:../secret.txt", 0, 0); return fd < 0 ? 7 : 8; }
EOF

echo "== compiling the guests with the vendored driver =="
./build/skjcc -target riscv -o confguest confguest.c
./build/skjcc -target riscv -o escguest  escguest.c

fail=0
check() {
    _label=$1; _want=$2; shift 2
    # these guests exit non-zero by design; keep set -e from killing the run
    "$@" >/dev/null 2>&1 && _got=0 || _got=$?
    if [ "$_got" = "$_want" ]; then
        echo "PASS: $_label (exit $_got)"
    else
        echo "FAIL: $_label (exit $_got, want $_want)"; fail=1
    fi
}

check "granted mount reads back"      42 ./build/skj-run --mount data:data confguest
check "escape above the mount denied"  7 ./build/skj-run --mount data:data escguest
check "no grant: scratch default"     10 ./build/skj-run confguest

[ "$fail" = 0 ] || { echo "vendor-smoke: FAILED"; exit 1; }
echo "vendor-smoke: a vendored sandbox tree builds and confines, end to end"
exit 0
