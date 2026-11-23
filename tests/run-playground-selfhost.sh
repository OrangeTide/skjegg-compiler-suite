#!/bin/sh
# run-playground-selfhost.sh - prove the browser playground pipeline on the host.
#
# For each playground language, drive the exact pipeline the browser workers run,
# but with the self-hosted compiler and linker wasms under wasmtime instead of in
# a Web Worker:
#   1. the self-hosted compiler wasm (skj-<lang>.wasm) compiles a sample program
#      to a WASM object (or, for TinScheme, a complete module);
#   2. for the object+link languages, the self-hosted linker wasm (skj-ld.wasm)
#      links that object against the language runtime objects;
#   3. the produced module runs under wasmtime and its stdout/exit is checked.
#
# If this passes, the same three stages work in the browser (same wasms, same
# argv, same memfs hand-off of the .o between stages).  A missing wasmtime skips
# the suite cleanly.
#
# Env:
#   SELFHOST_DIR - build/wasm-selfhost (holds skj-*.wasm and rt/*.o)
#   WASMTIME     - the wasm runtime (default wasmtime)
#   OUTDIR       - scratch dir (default build/playground-selfhost)
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
SELFHOST_DIR="${SELFHOST_DIR:-$ROOT/build/wasm-selfhost}"
WASMTIME="${WASMTIME:-wasmtime}"
OUTDIR="${OUTDIR:-$ROOT/build/playground-selfhost}"

if ! command -v "$WASMTIME" >/dev/null 2>&1; then
    echo "run-playground-selfhost: wasmtime not found, skipping"
    exit 0
fi

rm -rf "$OUTDIR"
mkdir -p "$OUTDIR"
RT="$SELFHOST_DIR/rt"
# the proposal flags are inert here but keep the invocation uniform with the
# scheme lane, which uses WasmFX stack switching for continuations.
WFLAGS="-W stack-switching=y -W function-references=y -W gc=y -W exceptions=y"

pass=0
fail=0

# compile+link+run one language.
#   $1 lang   $2 compiler wasm   $3 source file   $4 runtime objects ("" = none)
#   $5 expected stdout (exact)   $6 expected exit code
#   $7 extra compiler args ("" = none; C passes -Iinclude)
#   $8 host header dir to stage as the memfs include/ dir ("" = none)
check_lang() {
    lang=$1; comp=$2; src=$3; rtobjs=$4; want_out=$5; want_rc=$6
    cflags=${7:-}; incdir=${8:-}
    d="$OUTDIR/$lang"
    mkdir -p "$d"
    cp "$SELFHOST_DIR/$comp" "$d/"
    cp "$src" "$d/in.src"
    [ -z "$rtobjs" ] || cp $rtobjs "$d/"
    # C resolves <stdio.h>/<stdarg.h> from the staged include/ tree the same way
    # the browser seeds them into the worker memfs; -Iinclude points the
    # self-hosted cc at it (the self-host build bakes in no default include dir).
    if [ -n "$incdir" ]; then
        rm -rf "$d/include"
        cp -R "$incdir" "$d/include"
    fi

    if [ -z "$rtobjs" ]; then
        # TinScheme: the compiler emits a complete module directly.
        if ! ( cd "$d" && "$WASMTIME" run --dir=.::. "$comp" \
                 $cflags -o prog.wasm in.src ) 2>"$d/compile.err"; then
            printf 'FAIL  %s (compile): %s\n' "$lang" "$(head -1 "$d/compile.err")"
            fail=$((fail + 1)); return
        fi
    else
        if ! ( cd "$d" && "$WASMTIME" run --dir=.::. "$comp" \
                 $cflags -o out.o in.src ) 2>"$d/compile.err"; then
            printf 'FAIL  %s (compile): %s\n' "$lang" "$(head -1 "$d/compile.err")"
            fail=$((fail + 1)); return
        fi
        objs=""
        for o in $rtobjs; do objs="$objs $(basename "$o")"; done
        cp "$SELFHOST_DIR/skj-ld.wasm" "$d/"
        if ! ( cd "$d" && "$WASMTIME" run --dir=.::. skj-ld.wasm \
                 -o prog.wasm out.o $objs ) 2>"$d/link.err"; then
            printf 'FAIL  %s (link): %s\n' "$lang" "$(head -1 "$d/link.err")"
            fail=$((fail + 1)); return
        fi
    fi

    set +e
    out=$( cd "$d" && "$WASMTIME" run $WFLAGS prog.wasm 2>/dev/null )
    rc=$?
    set -e
    if [ "$rc" != "$want_rc" ]; then
        printf 'FAIL  %s (exit %s, want %s)\n' "$lang" "$rc" "$want_rc"
        fail=$((fail + 1)); return
    fi
    if [ "$out" != "$want_out" ]; then
        printf 'FAIL  %s (stdout mismatch)\n' "$lang"
        printf '  got:  %s\n  want: %s\n' "$out" "$want_out"
        fail=$((fail + 1)); return
    fi
    printf 'PASS  %s\n' "$lang"
    pass=$((pass + 1))
}

S="$OUTDIR/src"
mkdir -p "$S"

cat > "$S/hello.scm" <<'EOF'
(display "scheme ")
(display (* 6 7))
(newline)
EOF

cat > "$S/hello.pas" <<'EOF'
program p;
var i: integer;
begin
  for i := 1 to 3 do
    writeln('pascal ', i);
end.
EOF

cat > "$S/hello.bas" <<'EOF'
DIM i AS INT
FOR i = 1 TO 3
  PRINT "basic "; i
NEXT
EOF

cat > "$S/hello.moo" <<'EOF'
verb main()
	var x: int = 6;
	var y: int = 7;
	return x * y;
endverb
EOF

cat > "$S/hello.c" <<'EOF'
#include <stdio.h>
int main(void) {
    for (int i = 1; i <= 3; i++)
        printf("c %d\n", i);
    return 0;
}
EOF

check_lang scheme skj-sc.wasm   "$S/hello.scm" "" \
    "$(printf 'scheme 42')" 0
check_lang pascal skj-pc.wasm   "$S/hello.pas" "$RT/pascal_rt.o" \
    "$(printf 'pascal 1\npascal 2\npascal 3')" 0
check_lang basic  skj-bc.wasm   "$S/hello.bas" "$RT/basic_rt.o $RT/arena_wasm.o" \
    "$(printf 'basic 1\nbasic 2\nbasic 3')" 0
check_lang moo    skj-mooc.wasm "$S/hello.moo" \
    "$RT/str.o $RT/list.o $RT/host_stub.o $RT/arena_wasm.o" \
    "" 42
# The C guest links against the full WASM libc (rt/c/*.o) so printf reaches
# stdout; the synthesized crt provides fd_write/_start.  Expand the glob into a
# single space-joined argument so it lands in check_lang's rtobjs slot.  The
# headers come from the staged include/ tree (-Iinclude), the host counterpart of
# the file bundle the browser seeds into the worker memfs.
c_rt=$(echo "$RT"/c/*.o)
check_lang c      skj-cc.wasm   "$S/hello.c" "$c_rt" \
    "$(printf 'c 1\nc 2\nc 3')" 0 "-Iinclude" "$ROOT/include"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
