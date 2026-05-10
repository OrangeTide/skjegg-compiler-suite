#!/bin/sh
# vendor-skjegg.sh — vendor selected skjegg compiler components
# Made by a machine. PUBLIC DOMAIN (CC0-1.0)
#
# Copy this script into your project and edit ORIGIN below if needed.
# Running it generates update-skjegg.sh with your component selection
# baked in for easy re-vendoring.
#
# Usage: vendor-skjegg.sh [-d DIR] [-r REF] [-u DIR] component ...
#        update-skjegg.sh                      (re-vendor)
#
# Backends:  coldfire  riscv
# Frontends: tinc  scheme  moo  pascal  cc
# Tools:     as  ld  cpp

set -eu

ORIGIN="https://github.com/OrangeTide/skjegg-compiler-suite.git"
COMPONENTS=""
DEST="skjegg"
REF="main"
UPDATE_DIR="."

die() { printf 'error: %s\n' "$*" >&2; exit 1; }

usage() {
    cat <<'USAGE'
Usage: vendor-skjegg.sh [-d DIR] [-r REF] [-u DIR] component ...
       update-skjegg.sh                      (re-vendor)

Backends:  coldfire  riscv
Frontends: tinc  scheme  moo  pascal  cc
Tools:     as  ld  cpp

Options:
  -d DIR   destination directory (default: skjegg)
  -r REF   git ref — branch or tag (default: main)
  -u DIR   install update-skjegg.sh to DIR (default: cwd)
  -h       show this help
USAGE
    exit 1
}

# ---- option parsing ----

_u_explicit=0
while getopts d:r:u:h opt; do
    case "$opt" in
        d) DEST="$OPTARG" ;;
        r) REF="$OPTARG" ;;
        u) UPDATE_DIR="$OPTARG"; _u_explicit=1 ;;
        *) usage ;;
    esac
done
shift $((OPTIND - 1))

if [ $# -gt 0 ]; then
    COMPONENTS="$*"
fi

if [ -z "$COMPONENTS" ]; then
    die "no components specified (see -h)"
fi

# ---- parse and validate components ----

has_cf=0 has_rv=0
has_tinc=0 has_scheme=0 has_moo=0 has_pascal=0 has_cc=0
has_as=0 has_ld=0 has_cpp=0

for comp in $COMPONENTS; do
    case "$comp" in
        coldfire) has_cf=1 ;;
        riscv)    has_rv=1 ;;
        tinc)     has_tinc=1 ;;
        scheme)   has_scheme=1 ;;
        moo)      has_moo=1 ;;
        pascal)   has_pascal=1 ;;
        cc)       has_cc=1 ;;
        as)       has_as=1 ;;
        ld)       has_ld=1 ;;
        cpp)      has_cpp=1 ;;
        *)        die "unknown component: $comp" ;;
    esac
done

has_fe=$((has_tinc + has_scheme + has_moo + has_pascal + has_cc))
has_be=$((has_cf + has_rv))

if [ "$has_fe" -gt 0 ] && [ "$has_be" -eq 0 ]; then
    die "frontends require at least one backend"
fi

need_cpp_lib=0
if [ "$has_cc" -eq 1 ] || [ "$has_cpp" -eq 1 ]; then
    need_cpp_lib=1
fi

# ---- fetch ----

command -v git >/dev/null || die "git is required"

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

printf 'Fetching %s (ref: %s) ...\n' "$ORIGIN" "$REF"
git clone --quiet --depth 1 --branch "$REF" "$ORIGIN" "$tmpdir/src"
commit=$(git -C "$tmpdir/src" rev-parse --short HEAD)
printf '  commit %s\n' "$commit"

S="$tmpdir/src"

# ---- copy files ----

rm -rf "$DEST"

# IR core (always present)
mkdir -p "$DEST/ir"
cp "$S/ir/ir.c" "$S/ir/ir.h" "$S/ir/util.c" "$S/ir/util.h" \
   "$S/ir/arena.c" "$S/ir/arena.h" "$DEST/ir/"

# backends
if [ "$has_cf" -eq 1 ]; then
    mkdir -p "$DEST/backend" "$DEST/runtime"
    cp "$S/backend/cf_emit.c" "$S/backend/regalloc_cf.c" "$DEST/backend/"
    cp "$S/runtime/start.S" "$DEST/runtime/"
fi
if [ "$has_rv" -eq 1 ]; then
    mkdir -p "$DEST/backend" "$DEST/runtime"
    cp "$S/backend/rv_emit.c" "$S/backend/regalloc_rv.c" "$DEST/backend/"
    cp "$S/runtime/start_rv.S" "$DEST/runtime/"
fi

# frontends
if [ "$has_tinc" -eq 1 ]; then
    mkdir -p "$DEST/tinc"
    cp "$S"/tinc/*.c "$S"/tinc/*.h "$DEST/tinc/"
fi
if [ "$has_scheme" -eq 1 ]; then
    mkdir -p "$DEST/scheme"
    cp "$S/scheme/lex.c" "$S/scheme/parse.c" "$S/scheme/print.c" \
       "$S/scheme/lower.c" "$S/scheme/main.c" "$S/scheme/gc.c" \
       "$S/scheme/gc.h" "$S/scheme/scheme.h" "$DEST/scheme/"
fi
if [ "$has_moo" -eq 1 ]; then
    mkdir -p "$DEST/moo" "$DEST/runtime"
    cp "$S"/moo/*.c "$S"/moo/*.h "$DEST/moo/"
    for f in "$S"/moo/*.md "$S"/moo/*.ebnf; do
        [ -e "$f" ] && cp "$f" "$DEST/moo/"
    done
    cp "$S/runtime/str.c" "$S/runtime/list.c" \
       "$S/runtime/host_stub.c" "$S/runtime/toy_host.c" "$DEST/runtime/"
fi
if [ "$has_pascal" -eq 1 ]; then
    mkdir -p "$DEST/pascal" "$DEST/runtime"
    cp "$S"/pascal/*.c "$S"/pascal/*.h "$DEST/pascal/"
    cp "$S/runtime/pascal_rt.c" "$DEST/runtime/"
fi
if [ "$has_cc" -eq 1 ]; then
    mkdir -p "$DEST/cc"
    cp "$S"/cc/*.c "$S"/cc/*.h "$DEST/cc/"
fi

# cpp library (needed by cc and cpp tool)
if [ "$need_cpp_lib" -eq 1 ]; then
    mkdir -p "$DEST/cpp"
    cp "$S/cpp/tok.c" "$S/cpp/macro.c" "$S/cpp/cond.c" "$S/cpp/dir.c" \
       "$S/cpp/cpp.h" "$S/cpp/internal.h" "$DEST/cpp/"
fi
if [ "$has_cpp" -eq 1 ]; then
    cp "$S/cpp/main.c" "$DEST/cpp/"
fi

# assembler (library + optional tool)
if [ "$has_as" -eq 1 ]; then
    mkdir -p "$DEST/as"
    cp "$S/as/lex.c" "$S/as/parse.c" "$S/as/encode.c" "$S/as/elf.c" \
       "$S/as/as.h" "$DEST/as/"
    cp "$S/as/main.c" "$DEST/as/"
fi

# linker (library + optional tool)
if [ "$has_ld" -eq 1 ]; then
    mkdir -p "$DEST/ld"
    cp "$S/ld/elf_read.c" "$S/ld/script.c" "$S/ld/link.c" \
       "$S/ld/elf_write.c" "$S/ld/mapfile.c" \
       "$S/ld/ld.h" "$S/ld/mapfile.h" "$DEST/ld/"
    cp "$S/ld/main.c" "$DEST/ld/"
fi

# LICENSE
if [ -f "$S/LICENSE" ]; then
    cp "$S/LICENSE" "$DEST/"
fi

# ---- generate update-skjegg.sh ----

if [ -n "$UPDATE_DIR" ]; then
    _upd_dir="$UPDATE_DIR"
else
    _upd_dir="$(dirname "$0")"
fi

if [ "$_u_explicit" -eq 1 ]; then
    _baked_upd="$UPDATE_DIR"
else
    _baked_upd=""
fi

tmp_upd=$(mktemp)
sed -e "s|^ORIGIN=.*|ORIGIN=\"$ORIGIN\"|" \
    -e "s|^COMPONENTS=.*|COMPONENTS=\"$COMPONENTS\"|" \
    -e "s|^DEST=.*|DEST=\"$DEST\"|" \
    -e "s|^REF=.*|REF=\"$REF\"|" \
    -e "s|^UPDATE_DIR=.*|UPDATE_DIR=\"$_baked_upd\"|" \
    "$0" > "$tmp_upd"
chmod +x "$tmp_upd"
mkdir -p "$_upd_dir"
mv "$tmp_upd" "$_upd_dir/update-skjegg.sh"

# ---- generate skjegg.mk ----

MK="$DEST/skjegg.mk"

emit_compiler() {
    _name="$1"; _fe="$2"; _be="$3"; _inc="$4"; _extra="${5:-}"
    if [ -n "$_extra" ]; then
        _all_src="\$(SKJ_IR) \$($_be) \$($_fe) $_extra"
    else
        _all_src="\$(SKJ_IR) \$($_be) \$($_fe)"
    fi
    {
        printf '\nSKJ_ALL += $(BUILD)/%s\n' "$_name"
        printf '$(BUILD)/%s: %s | $(BUILD)\n' "$_name" "$_all_src"
        printf '\t$(CC) $(CFLAGS) %s -o $@ %s\n' "$_inc" "$_all_src"
    } >> "$MK"
}

# header
cat > "$MK" <<EOF
# skjegg.mk — vendored skjegg compiler toolkit
# Origin: $ORIGIN ($commit)
# Components: $COMPONENTS
# Generated: $(date -u +%Y-%m-%d)
#
# Include from your Makefile:
#   include $DEST/skjegg.mk
#
# Then build with:
#   make skjegg

SKJEGG := \$(dir \$(lastword \$(MAKEFILE_LIST)))
BUILD  ?= build

CC     ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Wpedantic -Wno-unused-parameter
EOF

# cross-toolchain variables
if [ "$has_cf" -eq 1 ]; then
    cat >> "$MK" <<'MK'

M68K_CC ?= m68k-linux-gnu-gcc
M68K_AS ?= m68k-linux-gnu-as
M68K_LD ?= m68k-linux-gnu-ld
MK
fi
if [ "$has_rv" -eq 1 ]; then
    cat >> "$MK" <<'MK'

RV_AS ?= riscv64-linux-gnu-as
RV_LD ?= riscv64-linux-gnu-ld
MK
fi

# source variables
{
    printf '\n# sources\n'
    printf 'SKJ_IR := $(SKJEGG)ir/ir.c $(SKJEGG)ir/util.c $(SKJEGG)ir/arena.c\n'
} >> "$MK"

if [ "$has_cf" -eq 1 ]; then
    printf 'SKJ_CF := $(SKJEGG)backend/regalloc_cf.c $(SKJEGG)backend/cf_emit.c\n' >> "$MK"
fi
if [ "$has_rv" -eq 1 ]; then
    printf 'SKJ_RV := $(SKJEGG)backend/regalloc_rv.c $(SKJEGG)backend/rv_emit.c\n' >> "$MK"
fi

if [ "$has_tinc" -eq 1 ]; then
    cat >> "$MK" <<'MK'
SKJ_TINC := $(SKJEGG)tinc/lex.c $(SKJEGG)tinc/parse.c \
            $(SKJEGG)tinc/lower.c $(SKJEGG)tinc/main.c
MK
fi
if [ "$has_scheme" -eq 1 ]; then
    cat >> "$MK" <<'MK'
SKJ_SCHEME := $(SKJEGG)scheme/lex.c $(SKJEGG)scheme/parse.c \
              $(SKJEGG)scheme/print.c $(SKJEGG)scheme/lower.c \
              $(SKJEGG)scheme/main.c $(SKJEGG)scheme/gc.c
MK
fi
if [ "$has_moo" -eq 1 ]; then
    cat >> "$MK" <<'MK'
SKJ_MOO := $(SKJEGG)moo/lex.c $(SKJEGG)moo/parse.c $(SKJEGG)moo/typecheck.c \
           $(SKJEGG)moo/lower.c $(SKJEGG)moo/main.c
MK
fi
if [ "$has_pascal" -eq 1 ]; then
    cat >> "$MK" <<'MK'
SKJ_PASCAL := $(SKJEGG)pascal/lex.c $(SKJEGG)pascal/parse.c \
              $(SKJEGG)pascal/lower.c $(SKJEGG)pascal/main.c
MK
fi
if [ "$has_cc" -eq 1 ]; then
    cat >> "$MK" <<'MK'
SKJ_CC := $(SKJEGG)cc/lex.c $(SKJEGG)cc/parse.c $(SKJEGG)cc/type.c \
          $(SKJEGG)cc/lower.c $(SKJEGG)cc/main.c
MK
fi
if [ "$need_cpp_lib" -eq 1 ]; then
    cat >> "$MK" <<'MK'
SKJ_CPP_LIB := $(SKJEGG)cpp/tok.c $(SKJEGG)cpp/macro.c \
               $(SKJEGG)cpp/cond.c $(SKJEGG)cpp/dir.c
MK
fi
if [ "$has_as" -eq 1 ]; then
    cat >> "$MK" <<'MK'
SKJ_AS_LIB := $(SKJEGG)as/lex.c $(SKJEGG)as/parse.c \
              $(SKJEGG)as/encode.c $(SKJEGG)as/elf.c
MK
fi
if [ "$has_ld" -eq 1 ]; then
    cat >> "$MK" <<'MK'
SKJ_LD_LIB := $(SKJEGG)ld/elf_read.c $(SKJEGG)ld/script.c \
              $(SKJEGG)ld/link.c $(SKJEGG)ld/elf_write.c \
              $(SKJEGG)ld/mapfile.c
MK
fi

# compiler binary rules (frontend x backend)
printf '\nSKJ_ALL :=\n' >> "$MK"

if [ "$has_tinc" -eq 1 ] && [ "$has_cf" -eq 1 ]; then
    emit_compiler skj-tinc SKJ_TINC SKJ_CF '-I$(SKJEGG)ir -I$(SKJEGG)tinc'
fi
if [ "$has_tinc" -eq 1 ] && [ "$has_rv" -eq 1 ]; then
    emit_compiler skj-tinc-rv SKJ_TINC SKJ_RV '-I$(SKJEGG)ir -I$(SKJEGG)tinc'
fi
if [ "$has_scheme" -eq 1 ] && [ "$has_cf" -eq 1 ]; then
    emit_compiler skj-sc SKJ_SCHEME SKJ_CF '-I$(SKJEGG)scheme -I$(SKJEGG)ir'
fi
if [ "$has_scheme" -eq 1 ] && [ "$has_rv" -eq 1 ]; then
    emit_compiler skj-sc-rv SKJ_SCHEME SKJ_RV '-I$(SKJEGG)scheme -I$(SKJEGG)ir'
fi
if [ "$has_moo" -eq 1 ] && [ "$has_cf" -eq 1 ]; then
    emit_compiler skj-mooc SKJ_MOO SKJ_CF '-I$(SKJEGG)moo -I$(SKJEGG)ir'
fi
if [ "$has_moo" -eq 1 ] && [ "$has_rv" -eq 1 ]; then
    emit_compiler skj-mooc-rv SKJ_MOO SKJ_RV '-I$(SKJEGG)moo -I$(SKJEGG)ir'
fi
if [ "$has_pascal" -eq 1 ] && [ "$has_cf" -eq 1 ]; then
    emit_compiler skj-pc SKJ_PASCAL SKJ_CF '-I$(SKJEGG)pascal -I$(SKJEGG)ir'
fi
if [ "$has_pascal" -eq 1 ] && [ "$has_rv" -eq 1 ]; then
    emit_compiler skj-pc-rv SKJ_PASCAL SKJ_RV '-I$(SKJEGG)pascal -I$(SKJEGG)ir'
fi
if [ "$has_cc" -eq 1 ] && [ "$has_cf" -eq 1 ]; then
    emit_compiler skj-cc SKJ_CC SKJ_CF \
        '-I$(SKJEGG)cc -I$(SKJEGG)cpp -I$(SKJEGG)ir' '$(SKJ_CPP_LIB)'
fi
if [ "$has_cc" -eq 1 ] && [ "$has_rv" -eq 1 ]; then
    emit_compiler skj-cc-rv SKJ_CC SKJ_RV \
        '-I$(SKJEGG)cc -I$(SKJEGG)cpp -I$(SKJEGG)ir' '$(SKJ_CPP_LIB)'
fi

# tool binary rules
if [ "$has_cpp" -eq 1 ]; then
    {
        printf '\nSKJ_ALL += $(BUILD)/skj-cpp\n'
        printf '$(BUILD)/skj-cpp: $(SKJEGG)cpp/main.c $(SKJ_CPP_LIB) $(SKJEGG)ir/util.c $(SKJEGG)ir/arena.c | $(BUILD)\n'
        printf '\t$(CC) $(CFLAGS) -I$(SKJEGG)cpp -I$(SKJEGG)ir -o $@ $(SKJEGG)cpp/main.c $(SKJ_CPP_LIB) $(SKJEGG)ir/util.c $(SKJEGG)ir/arena.c\n'
    } >> "$MK"
fi
if [ "$has_as" -eq 1 ]; then
    {
        printf '\nSKJ_ALL += $(BUILD)/skj-as\n'
        printf '$(BUILD)/skj-as: $(SKJEGG)as/main.c $(SKJ_AS_LIB) $(SKJEGG)ir/util.c $(SKJEGG)ir/arena.c | $(BUILD)\n'
        printf '\t$(CC) $(CFLAGS) -I$(SKJEGG)as -I$(SKJEGG)ir -o $@ $(SKJEGG)as/main.c $(SKJ_AS_LIB) $(SKJEGG)ir/util.c $(SKJEGG)ir/arena.c\n'
    } >> "$MK"
fi
if [ "$has_ld" -eq 1 ]; then
    {
        printf '\nSKJ_ALL += $(BUILD)/skj-ld\n'
        printf '$(BUILD)/skj-ld: $(SKJEGG)ld/main.c $(SKJ_LD_LIB) $(SKJEGG)ir/util.c $(SKJEGG)ir/arena.c | $(BUILD)\n'
        printf '\t$(CC) $(CFLAGS) -I$(SKJEGG)ld -I$(SKJEGG)ir -o $@ $(SKJEGG)ld/main.c $(SKJ_LD_LIB) $(SKJEGG)ir/util.c $(SKJEGG)ir/arena.c\n'
    } >> "$MK"
fi

# cross-compiled runtime objects
if [ "$has_cf" -eq 1 ]; then
    {
        printf '\n$(BUILD)/start.o: $(SKJEGG)runtime/start.S | $(BUILD)\n'
        printf '\t$(M68K_AS) -o $@ $<\n'
    } >> "$MK"
fi
if [ "$has_rv" -eq 1 ]; then
    {
        printf '\n$(BUILD)/start_rv.o: $(SKJEGG)runtime/start_rv.S | $(BUILD)\n'
        printf '\t$(RV_AS) -march=rv32im -mabi=ilp32 -o $@ $<\n'
    } >> "$MK"
fi
if [ "$has_pascal" -eq 1 ] && [ "$has_cf" -eq 1 ]; then
    {
        printf '\n$(BUILD)/pascal_rt.o: $(SKJEGG)runtime/pascal_rt.c | $(BUILD)\n'
        printf '\t$(M68K_CC) -std=c99 -O2 -Wall -ffreestanding -c -o $@ $<\n'
    } >> "$MK"
fi
if [ "$has_moo" -eq 1 ] && [ "$has_cf" -eq 1 ]; then
    {
        printf '\n$(BUILD)/str.o: $(SKJEGG)runtime/str.c | $(BUILD)\n'
        printf '\t$(M68K_CC) -std=c99 -O2 -Wall -ffreestanding -c -o $@ $<\n'
        printf '\n$(BUILD)/list.o: $(SKJEGG)runtime/list.c | $(BUILD)\n'
        printf '\t$(M68K_CC) -std=c99 -O2 -Wall -ffreestanding -c -o $@ $<\n'
        printf '\n$(BUILD)/host_stub.o: $(SKJEGG)runtime/host_stub.c | $(BUILD)\n'
        printf '\t$(M68K_CC) -std=c99 -O2 -Wall -ffreestanding -c -o $@ $<\n'
        printf '\n$(BUILD)/toy_host.o: $(SKJEGG)runtime/toy_host.c | $(BUILD)\n'
        printf '\t$(M68K_CC) -std=c99 -O2 -Wall -ffreestanding -c -o $@ $<\n'
    } >> "$MK"
fi

# phony and directory targets
cat >> "$MK" <<'MK'

skjegg: $(SKJ_ALL)
.PHONY: skjegg

$(BUILD):
	mkdir -p $@
MK

# ---- summary ----

printf '\nVendored into %s/ (%s)\n' "$DEST" "$commit"
printf 'Components: %s\n' "$COMPONENTS"
printf 'Generated:  %s/skjegg.mk\n' "$DEST"
printf 'Update:     %s/update-skjegg.sh\n' "$_upd_dir"
