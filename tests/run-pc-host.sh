#!/bin/sh
# Run the Pascal test suite natively on a host backend.
#
# The front end emits native assembly; this script assembles, links against the
# host Pascal runtime, runs the program (directly or under an emulator), and
# compares stdout to the test's .expected and the exit status to its .exitcode.
#
# Env:
#   MODE   x64 | a64
#   PC     the skj-pc binary for this target
#   RT     the host runtime object for this target
#   NASM   x64: the NASM assembler            (default: nasm)
#   CC     x64: the host C linker driver       (default: cc)
#   A64CC  a64: the aarch64 cross cc           (default: aarch64-linux-gnu-gcc)
#   QEMU   a64: the user-mode emulator         (default: qemu-aarch64)
#   SAN    x64 only: 1 links the generated program with AddressSanitizer and
#          LeakSanitizer so each test also fails on a leak, double free or
#          use after free in the heap managed runtime (RT must be the ASan
#          runtime object). The sanitizer report is printed for a failing test.
# Args: the .pas files to run.

set -u
MODE=${MODE:-x64}
PC=${PC:?PC not set}
RT=${RT:?RT not set}
NASM=${NASM:-nasm}
CC=${CC:-cc}
A64CC=${A64CC:-aarch64-linux-gnu-gcc}
QEMU=${QEMU:-qemu-aarch64}
SAN=${SAN:-0}

sanlink=""
asanopt=""
if [ "$SAN" = 1 ]; then
    sanlink="-fsanitize=address"
    asanopt="detect_leaks=1:halt_on_error=1"
fi

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

pass=0
fail=0
failed=""

for pas in "$@"; do
    b=$(basename "$pas" .pas)
    exp="$(dirname "$pas")/$b.expected"
    [ -f "$exp" ] || continue
    bin="$W/$b"
    if [ "$MODE" = x64 ]; then
        if ! "$PC" -o "$W/$b.asm" "$pas" 2>"$W/$b.err"; then
            failed="$failed $b(compile)"; fail=$((fail+1)); continue
        fi
        if ! "$NASM" -f elf64 "$W/$b.asm" -o "$W/$b.o" 2>"$W/$b.err"; then
            failed="$failed $b(asm)"; fail=$((fail+1)); continue
        fi
        if ! "$CC" -no-pie $sanlink "$W/$b.o" "$RT" -lm -o "$bin" 2>"$W/$b.err"; then
            failed="$failed $b(link)"; fail=$((fail+1)); continue
        fi
        runner=""
    else
        if ! "$PC" -o "$W/$b.s" "$pas" 2>"$W/$b.err"; then
            failed="$failed $b(compile)"; fail=$((fail+1)); continue
        fi
        if ! "$A64CC" -static "$W/$b.s" "$RT" -lm -o "$bin" 2>"$W/$b.err"; then
            failed="$failed $b(link)"; fail=$((fail+1)); continue
        fi
        runner="$QEMU"
    fi

    inp="$(dirname "$pas")/$b.input"
    rerr="$W/$b.run.err"
    if [ -f "$inp" ]; then
        out=$(ASAN_OPTIONS="$asanopt" $runner "$bin" <"$inp" 2>"$rerr"); rc=$?
    else
        out=$(ASAN_OPTIONS="$asanopt" $runner "$bin" </dev/null 2>"$rerr"); rc=$?
    fi

    ok=1
    [ "$out" = "$(cat "$exp")" ] || ok=0
    ec="$(dirname "$pas")/$b.exitcode"
    if [ -f "$ec" ]; then
        [ "$rc" = "$(cat "$ec")" ] || ok=0
    fi
    if [ "$SAN" = 1 ] && grep -q "Sanitizer" "$rerr" 2>/dev/null; then
        ok=0
    fi
    if [ $ok = 1 ]; then
        pass=$((pass+1))
    else
        failed="$failed $b"; fail=$((fail+1))
        if [ "$SAN" = 1 ] && [ -s "$rerr" ]; then
            sed "s/^/  $b: /" "$rerr" >&2
        fi
    fi
done

echo "pc-host ($MODE): PASS=$pass FAIL=$fail"
if [ $fail -ne 0 ]; then
    echo "FAILED:$failed"
    exit 1
fi
