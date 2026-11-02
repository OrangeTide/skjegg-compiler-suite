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
   --arch riscv --frontend cc addr2line \
   as-x86-64 ld-x86-64 as-aarch64 ld-arm64 \
   --with toolchain,runtime,sandbox,emulator >/dev/null

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
# setjmp/longjmp live in the vendored libc.a (vendor.sh archives setjmp_<arch>.o);
# a program that uses them, like the jsh shell, needs no extra object.  A longjmp
# of 42 must surface as setjmp's return, so main exits 42.
cat > sjguest.c <<'EOF'
#include <setjmp.h>
static jmp_buf jb;
int main(void)
{
    int v = setjmp(jb);
    if (v == 0)
        longjmp(jb, 42);
    return v;
}
EOF

echo "== compiling the guests with the vendored driver =="
./build/skjcc -target riscv -o confguest confguest.c
./build/skjcc -target riscv -o escguest  escguest.c
./build/skjcc -target riscv -o sjguest   sjguest.c

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
check "setjmp/longjmp from vendored libc.a" 42 ./build/skj-run sjguest

# The vDSO: the vendored tree builds the image (vdso-rv.elf is in SKJ_ALL) and
# skj-run injects it with --vdso.  The guest finds the table through environ
# (so this also guards getenv.c being in the vendored libc), calls the leaf
# functions, and reads the system page; vdso_test.c returns 0 only when every
# check passes.  Asserting on the exit code, the channel this test trusts.
echo "== building and running the vendored vDSO =="
./build/skjcc -target riscv -Iskjegg/include -o vdsoguest "$REPO/examples/vdso_test.c"
check "vDSO injected, table and system page valid" 0 \
    ./build/skj-run --vdso build/vdso-rv.elf vdsoguest

# The symbolized fault backtrace: build a nested assert failure with -g and run
# it.  skj-run exits 134 and, only when SKJ_DWARF_BACKTRACE is compiled in and
# the DWARF reader is vendored, prints a backtrace naming the guest functions.
# This guards the debugger-tier wiring (dwread.c + the flag), not just the build.
echo "== building and running a symbolized fault backtrace =="
./build/skjcc -target riscv -g -o aborts "$REPO/tests/debug/aborts.c"
./build/skj-run aborts 2>aborts.err && bt_ec=0 || bt_ec=$?
if [ "$bt_ec" = 134 ] && grep -q "backtrace:" aborts.err \
   && grep -q "deep" aborts.err && grep -q "main" aborts.err; then
    echo "PASS: symbolized fault backtrace (exit 134, names deep..main)"
else
    echo "FAIL: fault backtrace (exit $bt_ec)"; cat aborts.err; fail=1
fi

# The offline symbolizer: the vendored skj-addr2line reads the same DWARF the
# backtrace uses, but from a static binary, so a consumer can resolve an address
# without running the guest.  Take a statement address from the -g aborts ELF's
# line table and assert the vendored skj-addr2line agrees with GNU addr2line on
# function and source basename:line.  This guards that the addr2line component
# vendors (dwread.c + the build rule) and links, and that the vendored driver
# forwards -g so DWARF reaches the output at all.
echo "== resolving an address with the vendored skj-addr2line =="
a2l_addr=$(riscv64-linux-gnu-objdump --dwarf=decodedline aborts 2>/dev/null \
    | awk '/aborts\.c/ && $3 ~ /^0x/ {print $3; exit}')
a2l_skj=$(./build/skj-addr2line -f -e aborts "$a2l_addr" 2>/dev/null | sed 's:.*/::')
a2l_gnu=$(riscv64-linux-gnu-addr2line -f -e aborts "$a2l_addr" 2>/dev/null | sed 's:.*/::')
if [ -n "$a2l_addr" ] && [ "$a2l_skj" = "$a2l_gnu" ]; then
    echo "PASS: skj-addr2line matches GNU at $a2l_addr ($(echo "$a2l_skj" | tr '\n' ' '))"
else
    echo "FAIL: skj-addr2line ($a2l_addr: skj[$a2l_skj] != gnu[$a2l_gnu])"; fail=1
fi

# The 64-bit ELF64 toolchains: the vendored assembler and linker pairs build
# with no external binutils.  Assemble, link and run a committed fixture that
# exercises the relocation forms each target emits; the program writes "ok" and
# exits 42.  Guards the as-x86-64/ld-x86-64 and as-aarch64/ld-arm64 vendor
# wiring (the shared ELF64 core, the per-tool sources, the skjegg.mk rules).
echo "== vendored ELF64 toolchains assemble, link and run =="
./build/skj-as-x86-64 -o x64.o "$REPO/tests/ld/hello_x86_64.s"
./build/skj-ld-x86-64 -o x64prog x64.o
# the guest exits 42 by design; keep set -e from aborting on it
qemu-x86_64 ./x64prog >x64.out 2>/dev/null && x64_ec=0 || x64_ec=$?
if [ "`cat x64.out`" = ok ] && [ "$x64_ec" = 42 ]; then
    echo "PASS: vendored skj-as-x86-64 + skj-ld-x86-64 (ok, exit 42)"
else
    echo "FAIL: vendored x86-64 toolchain (out '`cat x64.out`', exit $x64_ec)"; fail=1
fi
./build/skj-as-aarch64 -o a64.o "$REPO/tests/ld/hello_arm64.s"
./build/skj-ld-arm64 -o a64prog a64.o
qemu-aarch64 ./a64prog >a64.out 2>/dev/null && a64_ec=0 || a64_ec=$?
if [ "`cat a64.out`" = ok ] && [ "$a64_ec" = 42 ]; then
    echo "PASS: vendored skj-as-aarch64 + skj-ld-arm64 (ok, exit 42)"
else
    echo "FAIL: vendored arm64 toolchain (out '`cat a64.out`', exit $a64_ec)"; fail=1
fi

# The hardware-virtualization arches vendor too.  With --with emulator an HV arch
# (x86-64 / aarch64) ships the HV core (hvcore.c + the KVM backend + the gdb stub)
# and builds EMU_HV=1, alongside any emulated cores.  Build-only here: running an
# HV guest needs /dev/kvm and a native guest, which a generic runner lacks, but
# the link (the vendor wiring that first broke this) is what we guard.
echo "== vendoring riscv,x86-64 --with emulator (emulated + HV) =="
sh "$work/vendor.sh" -d "$work/hv/skjegg" -u "$work/hv" -r "$ref" \
   --arch riscv,x86-64 --with emulator >/dev/null
( cd "$work/hv"
  printf 'include skjegg/skjegg.mk\nbuild:\n\tmkdir -p build\nall: $(SKJ_ALL)\n' > Makefile
  make all >hv-build.log 2>&1 ) \
  && echo "PASS: emulated+HV vendored tree builds (EMU_HV=1)" \
  || { echo "FAIL: emulated+HV vendored build"; cat "$work/hv/hv-build.log"; fail=1; }

# The 64-bit cc backend + runtime: vendor the full x86-64 and AArch64 C targets
# and drive the skjcc driver end to end with no external toolchain.  The vendored
# tree builds skj-cc-x86-64/skj-cc-arm64, the assemblers and ELF64 linkers, and
# self-hosts the crt and runtime helpers (skj-cc compiles va/half, skj-as
# assembles them); a C program compiles, links and runs (returns 45).  Guards
# the cc-backend copy, the SKJ_X64/SKJ_A64 vars, the emit_compiler calls and the
# 64-bit driver-lib staging.
echo "== vendoring x86-64,aarch64 cc backend + runtime (skjcc end to end) =="
sh "$work/vendor.sh" -d "$work/cc64/skjegg" -u "$work/cc64" -r "$ref" \
   --arch x86-64,aarch64 --frontend cc --with toolchain,runtime >/dev/null
( cd "$work/cc64"
  printf 'include skjegg/skjegg.mk\nbuild:\n\tmkdir -p build\nall: $(SKJ_ALL)\n' > Makefile
  make all >cc64.log 2>&1 ) \
  || { echo "FAIL: 64-bit cc vendored build"; cat "$work/cc64/cc64.log"; fail=1; }
if [ -x "$work/cc64/build/skjcc" ]; then
    printf 'int main(void){int s=0;for(int i=1;i<=9;i++)s+=i;return s;}\n' \
        > "$work/cc64/ret.c"
    ( cd "$work/cc64"
      ./build/skjcc -target x86-64 -o rx64 ret.c
      ./build/skjcc -target arm64 -o ra64 ret.c )
    qemu-x86_64 "$work/cc64/rx64" >/dev/null 2>&1 && x64_ec=0 || x64_ec=$?
    qemu-aarch64 "$work/cc64/ra64" >/dev/null 2>&1 && a64_ec=0 || a64_ec=$?
    if [ "$x64_ec" = 45 ] && [ "$a64_ec" = 45 ]; then
        echo "PASS: vendored skjcc builds and runs C for x86-64 and arm64 (exit 45)"
    else
        echo "FAIL: vendored skjcc (x86-64 exit $x64_ec, arm64 exit $a64_ec)"; fail=1
    fi
fi

[ "$fail" = 0 ] || { echo "vendor-smoke: FAILED"; exit 1; }
echo "vendor-smoke: a vendored sandbox tree builds and confines, end to end"
exit 0
