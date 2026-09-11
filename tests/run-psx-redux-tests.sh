#!/bin/sh
# run-psx-redux-tests.sh : functional test of skj-ld-mips PS-EXE output on the
# PCSX-Redux emulator with a real PlayStation BIOS.
#
# This is the functional counterpart of the structural run-psx-tests.sh.  Each
# C test is compiled, assembled, and linked to a PS-EXE with the BIOS-TTY crt,
# then run on PCSX-Redux booting a real BIOS ROM.  The crt's exit path writes
# main's return value to the PCSX-Redux "software exit" register (pcsx_exit,
# 0x1f802082), so in test mode the emulator quits with that code; the runner
# compares it to the test's .exitcode.  This actually runs the PlayStation
# executable, closing the gap the structural check leaves.
#
# Opt-in: it needs PCSX-Redux and a BIOS ROM, so it is skipped (exit 0) when
# either is missing.  Pass the BIOS with PSX_BIOS=/path/to/scphXXXX.bin.
#
# Env: CCBIN, ASM ("skj-as-mips -o"), LD ("skj-ld-mips -f ps-exe -o"),
#      START (start_psx.o), HALF, OUTDIR, PCSX_REDUX, PSX_BIOS.
# Args: test basenames (e.g. cc_t001_return), each with a tests/<name>.exitcode.

HERE=$(dirname "$0")
PCSX_REDUX=${PCSX_REDUX:-pcsx-redux}
OUTDIR=${OUTDIR:-build/psx-redux}

if ! command -v "$PCSX_REDUX" >/dev/null 2>&1; then
    echo "SKIP: PCSX-Redux ($PCSX_REDUX) not found"
    exit 0
fi
if [ -z "$PSX_BIOS" ] || [ ! -f "$PSX_BIOS" ]; then
    echo "SKIP: no PlayStation BIOS (pass PSX_BIOS=/path/to/scphXXXX.bin)"
    exit 0
fi

mkdir -p "$OUTDIR"
pass=0
fail=0

for name in "$@"; do
    src="$HERE/$name.c"
    exitcode_file="$HERE/$name.exitcode"
    [ -f "$src" ] || { printf 'SKIP  %s (no source)\n' "$name"; continue; }
    [ -f "$exitcode_file" ] || { printf 'SKIP  %s (no .exitcode)\n' "$name"; continue; }
    expect_rc=$(cat "$exitcode_file")

    asm="$OUTDIR/$name.s"
    obj="$OUTDIR/$name.o"
    exe="$OUTDIR/$name.exe"

    set +e
    "$CCBIN" -o "$asm" "$src" 2>/dev/null || { printf 'FAIL  %s (compile)\n' "$name"; fail=$((fail+1)); set -e; continue; }
    $ASM "$obj" "$asm" 2>/dev/null || { printf 'FAIL  %s (assemble)\n' "$name"; fail=$((fail+1)); set -e; continue; }
    $LD "$exe" "$START" "$obj" $HALF $EXTRA 2>/dev/null || { printf 'FAIL  %s (link)\n' "$name"; fail=$((fail+1)); set -e; continue; }

    # -testmode makes pcsx_exit terminate the process with the guest's code;
    # -cli uses the text UI so no OpenGL window is needed.  A timeout guards a
    # guest that never signals exit.
    timeout 60 "$PCSX_REDUX" -testmode -cli -run -no-gui-log \
        -bios "$PSX_BIOS" -exe "$exe" >/dev/null 2>&1
    rc=$?
    set -e

    if [ "$rc" -eq "$expect_rc" ]; then
        printf 'PASS  %s (exit %d)\n' "$name" "$rc"
        pass=$((pass + 1))
    else
        printf 'FAIL  %s (exit %d, want %d)\n' "$name" "$rc" "$expect_rc"
        fail=$((fail + 1))
    fi
done

echo ""
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
