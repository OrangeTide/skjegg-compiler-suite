# Makefile : build the Skjegg compiler tools and their test programs.
#
# Host build:         make          -> build/skj-tinc, build/skj-sc,
#                                      build/skj-mooc
# Target build+run:   make check    -> assembles/links each tests/*.tc
#                                      with m68k-linux-gnu-{as,ld} and
#                                      runs the result under qemu-m68k.
#                     make check-rv -> same but for RISC-V (RV32IM)
#                                      with riscv64-linux-gnu-{as,ld}
#                                      and qemu-riscv32.
#
# Toolchain overrides:
#   CC         host C compiler
#   M68K_CC    cross C compiler (default: m68k-linux-gnu-gcc)
#   M68K_AS    cross assembler  (default: m68k-linux-gnu-as)
#   M68K_LD    cross linker     (default: m68k-linux-gnu-ld)
#   QEMU       qemu user-mode   (default: qemu-m68k)
#   RV_AS      RISC-V cross assembler
#   RV_LD      RISC-V cross linker
#   QEMU_RV    qemu-riscv32
#   QEMU_RV_CPU  qemu -cpu string matching the rv32.c emulator (run-rv)

CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -Wpedantic -Wno-unused-parameter
CFLAGS  += -Ibuild

# Default search path for skj-provided headers (<stdarg.h>), baked into every
# cc front end.  SKJ_CC_INCLUDE overrides it at run time for a relocated build.
SKJ_INC_DEF := -DSKJ_INCLUDE_DIR='"$(CURDIR)/include"'

# AddressSanitizer + UndefinedBehaviorSanitizer, used by check-emu-san to
# rebuild the host-only emulator tests.  They drive emu/*.c natively and run
# in milliseconds, so this is a cheap net under the address arithmetic in the
# guest allocator and the scheduler.  -fno-sanitize-recover makes the first
# fault the exit status rather than a printed warning the run walks past.
SAN_CFLAGS := -fsanitize=address,undefined -fno-sanitize-recover=all -g

# Version: version.mk is authoritative, git only refines it.  In a release
# tree the tag v$(SKJ_VERSION) exists and describe adds the commit count and
# -dirty suffix.  With no matching tag we mark the build -dev.  Outside a git
# repo (tarball, vendored tree) SKJ_VERSION stands alone.
include version.mk
GIT_DESC := $(shell git describe --tags --dirty --match 'v$(SKJ_VERSION)' 2>/dev/null)
ifeq ($(GIT_DESC),)
GIT_SUFFIX := $(shell git describe --always --dirty 2>/dev/null | sed 's/^/-dev-g/')
else
GIT_SUFFIX := $(patsubst v$(SKJ_VERSION)%,%,$(GIT_DESC))
endif
VERSION := $(SKJ_VERSION)$(GIT_SUFFIX)
M68K_CC ?= m68k-linux-gnu-gcc
M68K_AS ?= m68k-linux-gnu-as
M68K_LD ?= m68k-linux-gnu-ld
M68K_ADDR2LINE ?= m68k-linux-gnu-addr2line
QEMU    ?= qemu-m68k
RV_AS   ?= riscv64-linux-gnu-as
RV_LD   ?= riscv64-linux-gnu-ld
RV_ADDR2LINE ?= riscv64-linux-gnu-addr2line
QEMU_RV ?= qemu-riscv32
# The ISA the rv32.c emulator implements: RV32IMAFC + Zicsr/Zifencei (on in the
# rv32 base CPU) plus Zba/Zbb/Zbs/Zcb (which the base CPU leaves off).  This is
# the closest qemu match, used by the `run-rv` target.  The one divergence is
# that qemu's rv32 CPU has D on where the emulator is single-precision only; the
# lockstep and fuzz harnesses handle that rather than turning D off.
QEMU_RV_CPU ?= rv32,zba=true,zbb=true,zbs=true,zcb=true
MIPS_AS ?= mipsel-none-elf-as
MIPS_LD ?= mipsel-none-elf-ld
MIPS_OBJCOPY ?= mipsel-none-elf-objcopy
MIPS_OBJDUMP ?= mipsel-none-elf-objdump
MIPS_ADDR2LINE ?= mipsel-linux-gnu-addr2line
QEMU_MIPS ?= qemu-mipsel
SPIM ?= spim
# The arm64 assembler: the in-tree build/skj-as-aarch64, which reads the GNU as
# syntax the arm64 backends emit and writes ELF64 EM_AARCH64 objects.  Override
# to the host cross assembler with A64_AS=aarch64-linux-gnu-as.  Linking stays
# external (A64_LD).
A64_AS  ?= build/skj-as-aarch64
A64_LD  ?= aarch64-linux-gnu-ld
A64_CC  ?= aarch64-linux-gnu-gcc
# Rules that run $(A64_AS) depend on $(A64_ASM_DEP) so the in-tree assembler is
# built first and dependents re-assemble when it changes; an override to the
# host as clears the dependency.
ifeq ($(A64_AS),build/skj-as-aarch64)
A64_ASM_DEP := build/skj-as-aarch64
else
A64_ASM_DEP :=
endif
QEMU_A64 ?= qemu-aarch64
# The in-process RV32 JIT driver is a hosted program (mmap/stdio/malloc), which
# the rv64 riscv64-linux-gnu toolchain cannot build for rv32 (no ilp32 glibc
# multilib).  zig cc bundles an rv32 libc, so it cross-builds the driver; it runs
# under qemu-riscv32.  Override RVJIT_CC if a native rv32 hosted toolchain exists.
RVJIT_CC ?= zig cc -target riscv32-linux-musl
# The x86-64 assembler: the in-tree build/skj-as-x86-64, which reads the GNU
# .intel_syntax noprefix the x86-64 backends emit and writes ELF64 objects.
# X64_ASMFLAGS carries any per-assembler flag; the in-tree tool needs none, so
# override both to use the host GNU `as` instead (X64_ASM=as X64_ASMFLAGS=--64).
# Every call site uses "$(X64_ASM) $(X64_ASMFLAGS)".  Linking stays external.
X64_ASM      ?= build/skj-as-x86-64
X64_ASMFLAGS ?=
X64_LD  ?= ld
# Rules that run $(X64_ASM) depend on $(X64_ASM_DEP) so the in-tree assembler is
# built before anything assembles with it, and dependents re-assemble when it
# changes (matching the skj-as-rv convention); an override to the host `as`
# (X64_ASM=as) clears the dependency.
ifeq ($(X64_ASM),build/skj-as-x86-64)
X64_ASM_DEP := build/skj-as-x86-64
else
X64_ASM_DEP :=
endif
# On an x86-64 Linux host the x86-64 target ELF is a native binary, so the test
# harness runs it directly and an empty QEMU_X64 means "run natively".  Any other
# host emulates it with qemu-x86_64.  Override on the command line (for example
# QEMU_X64=qemu-x86_64) to force the emulator on an x86-64 host.
QEMU_X64 ?= $(if $(filter Linux-x86_64,$(shell uname -s)-$(shell uname -m)),,qemu-x86_64)

IR_SRC  := ir/ir.c ir/util.c ir/arena.c
BE_CF   := backend/regalloc_cf.c backend/cf_emit.c
# RISC-V has one convention (the ILP32 psABI; the stack convention was retired),
# so the shared selector + GAS text sink replace rv_emit.c for every RV tool and
# integration test.  The JIT (jit/jit_rv.c byte sink) drives the same selector.
BE_RV   := backend/regalloc_rv.c backend/rv_select.c backend/rv_mc_text.c
BE_MIPS := backend/regalloc_mips.c backend/mips_emit.c
BE_A64  := backend/regalloc_arm64.c backend/arm64_emit.c
BE_X64  := backend/regalloc_x86.c backend/x86_emit.c

# AArch64 selector + GAS text sink, driven by the shared AArch64 selector.
# skj-cc-arm64 uses this; the stack-ABI arm64 tools (tinc/scheme)
# and the i64/ops/fpu integration tests keep BE_A64 (arm64_emit.c), the same
# split x86 has between BE_X64_SEL and BE_X64.
BE_A64_SEL := backend/regalloc_arm64.c backend/arm64_select.c backend/arm64_mc_text.c
# The unified x86-64 (LP64) backend: the shared instruction selector over the
# GNU intel-syntax text sink, driven by the AOT driver.  skj-cc-x86-64 uses this; the other
# x86-64 tools still use BE_X64 (x86_emit.c) until M3 moves them over.
BE_X64_SEL := backend/regalloc_x86.c backend/x86_select.c backend/mc_text.c \
              backend/x86_aot.c
TC_SRC  := tinc/lex.c tinc/parse.c tinc/lower.c tinc/main.c
SRC     := $(IR_SRC) $(BE_CF) $(TC_SRC)
SRC_RV  := $(IR_SRC) $(BE_RV) $(TC_SRC)
SRC_MIPS := $(IR_SRC) $(BE_MIPS) $(TC_SRC)
SRC_A64 := $(IR_SRC) $(BE_A64) $(TC_SRC)
SRC_X64 := $(IR_SRC) $(BE_X64) $(TC_SRC)

TESTS     := $(wildcard tests/*.tc)
SCM_TESTS := $(wildcard tests/scm_*.scm)
MOO_TESTS := $(filter-out tests/moo_room.moo tests/moo_toy_%.moo,$(wildcard tests/moo_*.moo))
MOO_TOY_TESTS := $(wildcard tests/moo_toy_*.moo)
PAS_TESTS := $(wildcard tests/pascal_*.pas)
# Floating-point tests (name ends in _real) need a libc-backed real formatter,
# which only the host backends have; they run via check-pc-host / check-pc-arm64
# and are excluded from the freestanding ColdFire binary builds and runs.
# Tests ending in _hostio depend on stdout and stderr being separate streams,
# which the host harness keeps apart but the VM harnesses merge (2>&1), so they
# are host-only too.
PAS_TESTS_NOFLOAT := $(filter-out %_real.pas %_hostio.pas,$(PAS_TESTS))

TOOLS := build/skj-tinc build/skj-tinc-rv build/skj-tinc-mips build/skj-tinc-arm64 \
         build/skj-sc build/skj-sc-rv build/skj-sc-mips build/skj-sc-arm64 \
         build/skj-mooc build/skj-pc build/skj-as build/skj-ld \
         build/skj-as-rv build/skj-ld-rv build/skj-as-mips build/skj-ld-mips build/skj-ar \
         build/skj-as-x86-64 build/skj-as-aarch64 \
         build/skj-cpp build/skj-cc build/skj-cc-x86-64 build/skj-cc-arm64 \
         build/skj-cc-rv build/skjcc build/skj-run \
         build/skj-addr2line build/skj-dbg \
         build/skj-jit build/skj-kp-jit build/skj-kp-patrol \
         build/skj-kp build/libkpaot.a build/skj-kpc \
         build/skj-kpc-rv build/skj-kpc-cf build/skj-kpc-arm64

all: $(TOOLS)

## help : print the common targets and the full check-*/test-* catalog.
## The catalog is generated from this Makefile, so it never falls out of date.
help:
	@echo "Skjegg build and test targets."
	@echo
	@echo "Common:"
	@echo "  make              build every tool (front ends, backends, toolchain)"
	@echo "  make check        ColdFire suite: compile, assemble, link, run under qemu-m68k"
	@echo "  make check-all    the whole qemu matrix across every backend, plus integration tests"
	@echo "  make check-smoke  full in-tree toolchain smoke test (no cross toolchain needed)"
	@echo "  make clean        remove build/"
	@echo
	@echo "All check-* and test-* targets (per backend, per feature, and the"
	@echo "emulator, MIPS, PlayStation, and self-host tiers):"
	@echo
	@grep -hoE '^(check|test)[A-Za-z0-9_-]*:' $(MAKEFILE_LIST) | sed 's/:$$//' \
	    | sort -u | column | sed 's/^/  /'

build/skj-tinc: $(SRC) ir/ir.h tinc/tinc.h | build
	$(CC) $(CFLAGS) -Iir -Itinc -o $@ $(SRC)

build/skj-tinc-rv: $(SRC_RV) ir/ir.h tinc/tinc.h | build
	$(CC) $(CFLAGS) -Iir -Itinc -o $@ $(SRC_RV)

build/skj-tinc-mips: $(SRC_MIPS) ir/ir.h tinc/tinc.h | build
	$(CC) $(CFLAGS) -Iir -Itinc -o $@ $(SRC_MIPS)

build/skj-tinc-arm64: $(SRC_A64) ir/ir.h tinc/tinc.h | build
	$(CC) $(CFLAGS) -DSKJ_LP64=1 -Iir -Itinc -o $@ $(SRC_A64)

build/skj-tinc-x86-64: $(SRC_X64) ir/ir.h tinc/tinc.h | build
	$(CC) $(CFLAGS) -DX86_BITS=64 -DSKJ_LP64=1 -Iir -Itinc -o $@ $(SRC_X64)

## C preprocessor
CPP_SRC := cpp/tok.c cpp/macro.c cpp/cond.c cpp/dir.c ir/util.c ir/arena.c

build/skj-cpp: cpp/main.c $(CPP_SRC) cpp/cpp.h cpp/internal.h | build
	$(CC) $(CFLAGS) -Icpp -Iir -o $@ cpp/main.c $(CPP_SRC)

## C compiler
CC_FE  := cc/lex.c cc/parse.c cc/type.c cc/lower.c cc/main.c
CC_CPP := cpp/tok.c cpp/macro.c cpp/cond.c cpp/dir.c
CC_SRC := $(CC_FE) $(CC_CPP) $(IR_SRC) $(BE_CF)
CC_SRC_X64 := $(CC_FE) $(CC_CPP) $(IR_SRC) $(BE_X64_SEL)
CC_SRC_A64 := $(CC_FE) $(CC_CPP) $(IR_SRC) $(BE_A64_SEL)
CC_SRC_RV := $(CC_FE) $(CC_CPP) $(IR_SRC) $(BE_RV)
CC_SRC_MIPS := $(CC_FE) $(CC_CPP) $(IR_SRC) $(BE_MIPS)

build/skj-cc: $(CC_SRC) cc/cc.h cpp/cpp.h cpp/internal.h ir/ir.h | build
	$(CC) $(CFLAGS) -DCC_M68K $(SKJ_INC_DEF) -Icc -Icpp -Iir -o $@ $(CC_SRC)

build/skj-cc-x86-64: $(CC_SRC_X64) cc/cc.h cpp/cpp.h cpp/internal.h ir/ir.h backend/mc.h backend/mc_text.h backend/x86_select.h | build
	$(CC) $(CFLAGS) -DX86_BITS=64 -DSKJ_LP64=1 -DCC_PSABI -DCC_STRUCT_ABI $(SKJ_INC_DEF) -Icc -Icpp -Iir -Ibackend -o $@ $(CC_SRC_X64)

build/skj-cc-arm64: $(CC_SRC_A64) cc/cc.h cpp/cpp.h cpp/internal.h ir/ir.h backend/arm64_mc.h backend/arm64_select.h backend/arm64_mc_text.h | build
	$(CC) $(CFLAGS) -DSKJ_LP64=1 -DCC_PSABI -DCC_ARM64 $(SKJ_INC_DEF) -Icc -Icpp -Iir -o $@ $(CC_SRC_A64)

build/skj-cc-rv: $(CC_SRC_RV) cc/cc.h cpp/cpp.h cpp/internal.h ir/ir.h | build
	$(CC) $(CFLAGS) -DCC_RISCV $(SKJ_INC_DEF) -Icc -Icpp -Iir -o $@ $(CC_SRC_RV)

## skjcc: the cross driver.  One command runs the per-target cc -> as -> ld
## pipeline and links the crt + runtime helpers from <libdir>/<target>/.  The
## compiled-in lib dir points at the in-tree staging tree (build/lib); the
## SKJ_CC_LIB env var overrides it, and the sub-tools are found beside skjcc.
build/skjcc: driver/main.c build/version.h | build
	$(CC) $(CFLAGS) -DSKJ_LIB_DIR='"$(CURDIR)/build/lib"' -o $@ driver/main.c

## skj-jit: the in-process x86-64 JIT.  The cc front end lowers a C source to
## an ir_program which the jit/ library compiles to native code and runs in
## the same process, with no assembler, linker, or emulator.  The front end is
## built ILP32-address + SysV register ABI (CC_PSABI, not CC_LP64), the model
## the JIT selector (jit/jit_x86.c) emits, over the shared register allocator.
## CC_STRUCT_ABI turns on the SysV eightbyte classification so a small struct is
## passed and returned in registers (the psABI struct-by-value rules), the same
## as skj-cc-x86-64.  jit/jit_x86.c is the embeddable library; jit/main.c is the
## driver over it.
JIT_FE  := cc/lex.c cc/parse.c cc/type.c cc/lower.c
JIT_SRC := jit/main.c jit/jit_common.c jit/jit_arena.c jit/jit_x86.c jit/emit_x86.c jit/code.c \
           jit/call_guest_x86.S \
           backend/x86_select.c backend/regalloc_x86.c $(JIT_FE) $(CC_CPP) $(IR_SRC)
build/skj-jit: $(JIT_SRC) cc/cc.h jit/jit_common.h jit/jit_arena.h jit/jit_x86.h jit/emit_x86.h jit/code.h backend/mc.h backend/x86_select.h ir/ir.h Makefile build/version.h | build
	$(CC) $(CFLAGS) -DX86_BITS=64 -DSKJ_LP64=1 -DCC_PSABI -DCC_STRUCT_ABI $(SKJ_INC_DEF) -Icc -Icpp -Iir -Ijit -Ibackend -o $@ $(JIT_SRC)

## Run the C suite in-process through skj-jit and compare exit codes, the
## counterpart of check-cc without an assembler, linker, or qemu.  The runner
## skips the tests whose features the JIT does not yet cover (see its EXCLUDE).
check-jit: build/skj-jit
	@sh tests/run-jit-tests.sh

## skj-kp-jit: the in-process x86-64 JIT for Kobold modules.  The Kobold front
## end stays a separate self-hosting Pascal program; its KPM1 text output is the
## bridge into this tree (kobold/kpm.c reads it into an ir_program).  The driver then
## drives the same selector and byte sink as skj-jit.  See doc/languages/kobold/kobold.md.
## The Kobold runtime ported into this tree: the guest-owned string object
## (str_core.c) over the host platform layer (rt_host.c: a low-memory guest heap,
## ARC, finalizers).  Compiled in the freestanding tier (KP_FREESTANDING), whose
## identity addressing matches the JIT's low-memory guest model.
## The ported Kobold runtime (strings, arrays, maps, coroutines, Str/Val, the
## guest heap and ARC) plus the shared embedder surface (kp_host.c) and the JIT.
## A driver's main.c links against this; skj-kp-jit is the generic runner and
## skj-kp-patrol a host that adds an opaque handle type.
KP_RT  := kobold/kpm.c kobold/str_core.c kobold/str_fmt.c kobold/dynarr.c kobold/map.c kobold/rt_host.c \
          kobold/co_shim.c runtime/co_switch_x86.S kobold/kp_host.c kobold/kp_io.c \
          jit/jit_common.c jit/jit_arena.c jit/jit_x86.c jit/emit_x86.c jit/code.c \
          jit/call_guest_x86.S \
          backend/x86_select.c backend/regalloc_x86.c $(IR_SRC)
KP_HDRS := kobold/kpm.h kobold/str.h kobold/dynarr.h kobold/map.h kobold/rt_glue.h kobold/obj.h kobold/gheap_core.h kobold/co.h \
           kobold/handle.h kobold/lowmem.h kobold/kp_host.h jit/jit_common.h jit/jit_arena.h \
           jit/jit_x86.h jit/emit_x86.h jit/code.h backend/mc.h backend/x86_select.h ir/ir.h ir/target.h
KP_CFLAGS := $(CFLAGS) -DX86_BITS=64 -DKP_FREESTANDING -DKP_REAL=double -Iir -Ijit -Ibackend -Ikobold

## co.c is vendored unchanged from the Kobold tree, which builds with -Wall -Wextra
## (no -Wpedantic).  Compile it with that same set so it stays byte-identical and
## re-syncable, while the rest of the kobold/ glue keeps Skjegg's full -Wpedantic.
build/kp_co.o: kobold/co.c kobold/co.h kobold/handle.h kobold/lowmem.h Makefile | build
	$(CC) -std=c99 -O2 -Wall -Wextra -Wno-unused-parameter -Iir -Ijit -Ibackend -Ikobold -c kobold/co.c -o $@

## bin2c embeds a bootstrap seed as a C byte array (build/kpcom_kpm.c, with a
## matching .h generated as a side effect) so skj-kp-jit* carry the Kobold front
## end in the binary.  Then 'skj-kp-jit foo.pas' JITs the embedded front end,
## compiles the source in process through its `compile` export, and runs the
## produced module.  KPCOM_KPM in the environment overrides it for dev iteration.
## Each JIT embeds its own target's seed (below): the rv JIT this build/kpcom_kpm.c
## (rv32), the x86-64 and arm64 JITs the per-arch seeds under build/x86-64 and
## build/arm64.  None is committed: skj-pc is the bootstrap root, so each front end
## is built from source and emits its own seed at build time.  The -D KP_TARGET_*
## flag at emit selects kpcom's target inside the seed (the architecture tag, from
## which the pointer width derives), so the embedded front end emits modules for
## the right target.
build/bin2c: kobold/bin2c.c Makefile | build
	$(CC) $(CFLAGS) -o $@ kobold/bin2c.c

build/kpcom_kpm.c: build/kpcom-rv.kpm build/bin2c | build
	build/bin2c kpcom_kpm build/kpcom-rv.kpm build/kpcom_kpm.c build/kpcom_kpm.h

## kpcom's source and its {$I} includes.  Defined here, before the first rule that
## uses it, because make expands a rule's prerequisites when the makefile is read:
## a `:=` variable defined later would expand empty here, so the front ends and
## seeds would not depend on the source and an edit would not rebuild them.
KPCOM_SRC_PAS := kobold/kpcom.pas
KPCOM_SRCS    := kobold/kpcom.pas kobold/version.inc kobold/stdlib.inc

## The per-architecture bootstrap seeds, generated at build time (not committed):
## each target's host front end emits its own source as that target's KPM1, stamped
## with its architecture (there is no shared ilp32 or lp64 seed).  skj-pc is the
## bootstrap root, so the front ends are built from source with no prior seed.  Each
## seed feeds that target's JIT (bin2c embeds it) and/or self-host ELF.
build/kpcom-x86-64.kpm: build/skj-kpc $(KPCOM_SRCS) | build
	build/skj-kpc -D KP_TARGET_X64 --emit-kpm $(KPCOM_SRC_PAS) > $@
build/kpcom-arm64.kpm: build/skj-kpc-arm64 $(KPCOM_SRCS) | build
	build/skj-kpc-arm64 -D KP_TARGET_ARM64 --emit-kpm $(KPCOM_SRC_PAS) > $@
build/kpcom-rv.kpm: build/skj-kpc-rv $(KPCOM_SRCS) | build
	build/skj-kpc-rv -D KP_TARGET_RV --emit-kpm $(KPCOM_SRC_PAS) > $@
build/kpcom-cf.kpm: build/skj-kpc-cf $(KPCOM_SRCS) | build
	build/skj-kpc-cf -D KP_TARGET_CF --emit-kpm $(KPCOM_SRC_PAS) > $@

## bin2c embeds the seed per architecture (same symbol and header name; each JIT
## compiles with its own -Ibuild/<arch> ahead of -Ibuild so frontend.c's
## `#include "kpcom_kpm.h"` picks up the matching seed).
build/x86-64/kpcom_kpm.c: build/kpcom-x86-64.kpm build/bin2c | build
	@mkdir -p build/x86-64
	build/bin2c kpcom_kpm build/kpcom-x86-64.kpm build/x86-64/kpcom_kpm.c build/x86-64/kpcom_kpm.h
build/arm64/kpcom_kpm.c: build/kpcom-arm64.kpm build/bin2c | build
	@mkdir -p build/arm64
	build/bin2c kpcom_kpm build/kpcom-arm64.kpm build/arm64/kpcom_kpm.c build/arm64/kpcom_kpm.h

## skj-kp-jit: the native (x86-64, LP64) in-process JIT.  Runtime and front end
## compiled for x86-64 (-DX86_BITS=64 in KP_CFLAGS) so guest pointers are full
## 64-bit; the embedded seed is the x86-64 one and kp_co is the LP64 coroutine
## object.
build/skj-kp-jit: kobold/jit_main.c kobold/frontend.c build/x86-64/kpcom_kpm.c $(KP_RT) build/kp_co_lp64.o kobold/frontend.h $(KP_HDRS) Makefile | build
	$(CC) $(KP_CFLAGS) -Ibuild/x86-64 -Ibuild -o $@ kobold/jit_main.c kobold/frontend.c build/x86-64/kpcom_kpm.c $(KP_RT) build/kp_co_lp64.o -lm

## kp_co_lp64.o: the coroutine core compiled LP64 (-DX86_BITS=64: native 64-bit
## guest pointer/handle, guest stacks in full memory), linked into the x86-64
## LP64 tools.
build/kp_co_lp64.o: kobold/co.c kobold/co.h kobold/handle.h kobold/lowmem.h kobold/obj.h Makefile | build
	$(CC) -std=c99 -O2 -Wall -Wextra -Wno-unused-parameter -DX86_BITS=64 -Iir -Ijit -Ibackend -Ikobold -c kobold/co.c -o $@

## skj-kpc: the native Kobold front end, the host compiler for the x86-64 target.
## Plan 9 style, the target baked at build: skj-pc compiles kpcom.pas with
## -d KP_TARGET_X64, so CurTarget is x86-64 (8-byte pointers, width derived from
## the target) and every module it emits is stamped `.target x86-64`.  No seed and
## no libkpaot; it links
## the host-import shim (ReadFileStr, WriteFileStr, kpd_*) and skj-pc's Pascal
## runtime, the same ones check-pc-kpcom uses.  skj-kpc-x86-64 is the explicit arch
## name for the same binary.
build/skj-kpc: build/skj-pc-x86-64 tests/pascal_kpcom_host.c build/pascal_rt_host.o $(KPCOM_SRCS) $(X64_ASM_DEP) | build
	build/skj-pc-x86-64 -d KOBOLD -d KP_TARGET_X64 -o build/kpcom-x86-64-fe.s $(KPCOM_SRC_PAS)
	$(X64_ASM) $(X64_ASMFLAGS) build/kpcom-x86-64-fe.s -o build/kpcom-x86-64-fe.o
	$(CC) -no-pie build/kpcom-x86-64-fe.o tests/pascal_kpcom_host.c build/pascal_rt_host.o -lm -o $@

## skj-kpc-arm64: the host front end for the AArch64 target.  Same LP64 width as
## skj-kpc, but KP_TARGET_ARM64 stamps `.target arm64`, so the arm64 backend accepts
## its modules and rejects the x86-64 ones.  It runs on the x86-64 host (skj-pc
## output); the arch it names is the target it emits for, not where it runs.
build/skj-kpc-arm64: build/skj-pc-x86-64 tests/pascal_kpcom_host.c build/pascal_rt_host.o $(KPCOM_SRCS) $(X64_ASM_DEP) | build
	build/skj-pc-x86-64 -d KOBOLD -d KP_TARGET_ARM64 -o build/kpcom-arm64-fe.s $(KPCOM_SRC_PAS)
	$(X64_ASM) $(X64_ASMFLAGS) build/kpcom-arm64-fe.s -o build/kpcom-arm64-fe.o
	$(CC) -no-pie build/kpcom-arm64-fe.o tests/pascal_kpcom_host.c build/pascal_rt_host.o -lm -o $@

build/skj-kpc-x86-64: build/skj-kpc | build
	cp -f build/skj-kpc $@


## skj-kpc-rv / skj-kpc-cf: the per-target host front ends for the rv32 and
## ColdFire backends.  Each is skj-pc compiling kpcom.pas with its target baked in
## (KP_TARGET_RV / KP_TARGET_CF), so the module it emits is stamped `.target rv32`
## or `.target cf` and is bound to that one architecture.  One compiler per target,
## the cc model: a unit built for rv32 is not a unit for ColdFire, so there is no
## shared "ilp32" front end.  (The architecture decides the pointer width; both
## happen to be 4 bytes, but that is a consequence, not a shared category.)
build/skj-kpc-rv: build/skj-pc-x86-64 tests/pascal_kpcom_host.c build/pascal_rt_host.o $(KPCOM_SRCS) $(X64_ASM_DEP) | build
	build/skj-pc-x86-64 -d KOBOLD -d KP_TARGET_RV -o build/kpcom-rv-fe.s $(KPCOM_SRC_PAS)
	$(X64_ASM) $(X64_ASMFLAGS) build/kpcom-rv-fe.s -o build/kpcom-rv-fe.o
	$(CC) -no-pie build/kpcom-rv-fe.o tests/pascal_kpcom_host.c build/pascal_rt_host.o -lm -o $@

build/skj-kpc-cf: build/skj-pc-x86-64 tests/pascal_kpcom_host.c build/pascal_rt_host.o $(KPCOM_SRCS) $(X64_ASM_DEP) | build
	build/skj-pc-x86-64 -d KOBOLD -d KP_TARGET_CF -o build/kpcom-cf-fe.s $(KPCOM_SRC_PAS)
	$(X64_ASM) $(X64_ASMFLAGS) build/kpcom-cf-fe.s -o build/kpcom-cf-fe.o
	$(CC) -no-pie build/kpcom-cf-fe.o tests/pascal_kpcom_host.c build/pascal_rt_host.o -lm -o $@

## skj-kp-jit-san: build/skj-kp-jit with the guest-heap write-after-free instrument
## (GH_POISON, kobold/rt_host.c) plus ASan/UBSan, at -O1 -g so a report carries a usable
## backtrace.  This is the tool that validates the string/array/map runtimes' reference
## counting: an over-release, double free, or use-after-free in an in-place mutator (for
## example the string self-append path, kp_str_append*) trips here even when the program's
## printed values happen to look right.  Kept out of `all`; built on demand by
## check-kp-corpus-san.
## It is the native (LP64) JIT, so a width-8 truncation (a 64-bit guest address
## narrowed to 32 bits dereferencing wild) also trips here under ASan.
build/skj-kp-jit-san: kobold/jit_main.c kobold/frontend.c build/x86-64/kpcom_kpm.c $(KP_RT) build/kp_co_lp64.o kobold/frontend.h $(KP_HDRS) Makefile | build
	$(CC) $(KP_CFLAGS) -Ibuild/x86-64 -O1 -g -DGH_POISON $(SAN_CFLAGS) -Ibuild -o $@ kobold/jit_main.c kobold/frontend.c build/x86-64/kpcom_kpm.c $(KP_RT) build/kp_co_lp64.o -lm

build/skj-kp-patrol: kobold/patrol_main.c $(KP_RT) build/kp_co.o $(KP_HDRS) Makefile | build
	$(CC) $(KP_CFLAGS) -o $@ kobold/patrol_main.c $(KP_RT) build/kp_co.o -lm

## skj-kp-jit-rv: the in-process RV32 JIT for Kobold modules, the cross-arch
## counterpart of skj-kp-jit.  Same driver, ported runtime, and selector; only the
## arch-specific byte sink (jit_rv/emit_rv), the stack-ABI i64 helpers, the call
## trampoline, and the coroutine switch differ.  rt_host/co_shim need no change: on
## a 32-bit host a plain mmap is low by construction, so the guest address model
## holds without MAP_32BIT.  Built with RVJIT_CC (zig's bundled rv32 libc, since the
## rv64 toolchain has no ilp32 multilib) and run under qemu-riscv32.
KP_RT_RV := kobold/kpm.c kobold/str_core.c kobold/str_fmt.c kobold/dynarr.c kobold/map.c kobold/rt_host.c \
            kobold/co_shim.c runtime/co_switch_rv.S kobold/kp_host.c kobold/kp_io.c \
            jit/jit_common.c jit/jit_arena.c jit/jit_rv.c jit/emit_rv.c jit/code.c \
            jit/call_guest_rv.S jit/rv_jit_i64.S \
            backend/rv_select.c backend/regalloc_rv.c $(IR_SRC)
KP_HDRS_RV := kobold/kpm.h kobold/str.h kobold/dynarr.h kobold/map.h kobold/rt_glue.h kobold/obj.h kobold/gheap_core.h kobold/co.h \
              kobold/handle.h kobold/lowmem.h kobold/kp_host.h jit/jit_common.h jit/jit_arena.h \
              jit/emit_rv.h jit/code.h backend/rv_mc.h backend/rv_select.h ir/ir.h ir/target.h
KP_CFLAGS_RV := $(CFLAGS) -DKP_FREESTANDING -DKP_REAL=double -Iir -Ijit -Ibackend -Ikobold

## co.c built for rv with the same vendored-warning set as the x86 kp_co.o.
build/kp_co_rv.o: kobold/co.c kobold/co.h kobold/handle.h kobold/lowmem.h Makefile | build
	$(RVJIT_CC) -std=c99 -O2 -Wall -Wextra -Wno-unused-parameter -Iir -Ijit -Ibackend -Ikobold -c kobold/co.c -o $@

build/skj-kp-jit-rv: kobold/jit_main.c kobold/frontend.c build/kpcom_kpm.c $(KP_RT_RV) build/kp_co_rv.o kobold/frontend.h $(KP_HDRS_RV) runtime/i64_rv.S Makefile | build
	$(RVJIT_CC) $(KP_CFLAGS_RV) -DSKJ_KPM_ARCH='"rv32"' -Ibuild -o $@ kobold/jit_main.c kobold/frontend.c build/kpcom_kpm.c $(KP_RT_RV) build/kp_co_rv.o -lm

## skj-kp-jit-arm64: the in-process AArch64 JIT for Kobold modules, the cross-arch
## counterpart of skj-kp-jit.  Same driver, ported runtime, and selector, over the
## AArch64 byte sink (jit_arm64/emit_arm64), call trampoline, and coroutine switch.
## i64 is native (no stack-ABI helpers) and floats ride the AAPCS64 d-registers, so
## no ABI bridge is needed, unlike rv32.  On this LP64 host a guest address must
## still be below 4GB, so the guest heap and coroutine stacks are mapped low (see
## rt_host.c/co_shim.c).  Cross-built with the arm64 toolchain and run under
## qemu-aarch64, which executes the JIT-generated code.
KP_RT_A64 := kobold/kpm.c kobold/str_core.c kobold/str_fmt.c kobold/dynarr.c kobold/map.c kobold/rt_host.c \
             kobold/co_shim.c runtime/co_switch_arm64.S kobold/kp_host.c kobold/kp_io.c \
             jit/jit_common.c jit/jit_arena.c jit/jit_arm64.c jit/emit_arm64.c jit/code.c \
             jit/call_guest_arm64.S \
             backend/arm64_select.c backend/regalloc_arm64.c $(IR_SRC)
KP_HDRS_A64 := kobold/kpm.h kobold/str.h kobold/dynarr.h kobold/map.h kobold/rt_glue.h kobold/obj.h kobold/gheap_core.h kobold/co.h \
               kobold/handle.h kobold/lowmem.h kobold/kp_host.h jit/jit_common.h jit/jit_arena.h \
               jit/emit_arm64.h jit/code.h backend/arm64_mc.h backend/arm64_select.h ir/ir.h
KP_CFLAGS_A64 := $(CFLAGS) -DKP_FREESTANDING -DKP_REAL=double -Iir -Ijit -Ibackend -Ikobold

## ColdFire shares the arm64/rv flag set (no -DX86_BITS); it cross-builds the
## hosted Kobold runtime for big-endian m68k with the glibc m68k toolchain.
KP_CFLAGS_CF := $(CFLAGS) -DKP_FREESTANDING -DKP_REAL=double -Iir -Ijit -Ibackend -Ikobold

## co.c built for arm64 with the same vendored-warning set as the x86 kp_co.o.
build/kp_co_a64.o: kobold/co.c kobold/co.h kobold/handle.h kobold/lowmem.h Makefile | build
	$(A64_CC) -std=c99 -O2 -Wall -Wextra -Wno-unused-parameter -Iir -Ijit -Ibackend -Ikobold -c kobold/co.c -o $@

## kp_co_a64_lp64.o: the coroutine core for arm64, compiled LP64 (-DCC_ARM64:
## native 64-bit guest pointer, guest stacks in full memory), linked into the
## arm64 LP64 JIT and runtime.
build/kp_co_a64_lp64.o: kobold/co.c kobold/co.h kobold/handle.h kobold/lowmem.h kobold/obj.h Makefile | build
	$(A64_CC) -std=c99 -O2 -Wall -Wextra -Wno-unused-parameter -DCC_ARM64 -Iir -Ijit -Ibackend -Ikobold -c kobold/co.c -o $@

## co.c built for ColdFire/m68k with the same vendored-warning set as kp_co.o.
build/kp_co_cf.o: kobold/co.c kobold/co.h kobold/handle.h kobold/lowmem.h Makefile | build
	$(M68K_CC) -std=c99 -O2 -Wall -Wextra -Wno-unused-parameter -Iir -Ijit -Ibackend -Ikobold -c kobold/co.c -o $@

## skj-kp-jit-arm64: the AArch64 in-process JIT.  AArch64 is an LP64 target, so the
## runtime and front end are compiled -DCC_ARM64 (8-byte pointers), the embedded
## seed is the arm64 one, and kp_co is the LP64 coroutine object.  Runs under
## qemu-aarch64.
build/skj-kp-jit-arm64: kobold/jit_main.c kobold/frontend.c build/arm64/kpcom_kpm.c $(KP_RT_A64) build/kp_co_a64_lp64.o kobold/frontend.h $(KP_HDRS_A64) Makefile | build
	$(A64_CC) $(KP_CFLAGS_A64) -DCC_ARM64 -Ibuild/arm64 -Ibuild -o $@ kobold/jit_main.c kobold/frontend.c build/arm64/kpcom_kpm.c $(KP_RT_A64) build/kp_co_a64_lp64.o -lm

## skj-kp: the x86-64 ahead-of-time compiler for Kobold modules.  It reads a KPM1
## module and writes GNU intel assembly through the same selector the JIT drives, over
## the text sink (backend/mc_text.c) and AOT driver (backend/x86_aot.c).  The output
## is assembled with the host `as` (intel mode) and linked -no-pie against libkpaot.a:
##     skj-kp prog.kpm -o prog.s
##     as --64 prog.s -o prog.o
##     cc -no-pie prog.o build/libkpaot.a -o prog
## AArch64 and x86-64 are LP64 targets: a guest pointer is a native 64-bit value and
## the runtime maps the guest heap and execution stack in full memory (no low-memory
## trick).  The x86 selector keeps every integer temp in a uniform 64-bit home, so
## skj-kp is built -DX86_BITS=64 (its LP64 width); the LP64-ness also lives in the
## KPM it reads and in libkpaot.a (built for x86-64 below).
build/skj-kp: kobold/aot_main.c kobold/kpm.c $(IR_SRC) $(BE_X64_SEL) kobold/kpm.h ir/ir.h \
              backend/mc_text.h backend/x86_select.h Makefile | build
	$(CC) $(CFLAGS) -DX86_BITS=64 -Iir -Ibackend -Ikobold -o $@ \
	    kobold/aot_main.c kobold/kpm.c $(IR_SRC) $(BE_X64_SEL)

## Exercise the KPM1 module reader's input validation (kobold/kpm.c) through skj-kp.
## Self-contained: no external Kobold repo, no qemu, no assembler or linker.  The
## cases live in tests/kp/ and follow the .experror convention (a malformed module
## must be rejected with the pinned message; a well-formed one must compile).
check-kp: build/skj-kp
	@sh tests/run-kp-tests.sh

## libkpaot.a: the Kobold runtime an AOT program links against.  It is the same
## ported runtime the JIT binds (strings, arrays, maps, coroutines, ARC, the guest
## heap, console I/O) compiled in the freestanding tier with f64 reals, plus the C
## start file kobold/aot_rt.c (process entry, low execution stack, traps, and the
## kprun-compatible debug surface) and kp_call_guest.  Built by name resolution at
## link rather than through a binding table.
## co.c is vendored and builds with -Wall -Wextra but not -Wpedantic, so it is taken
## from the shared build/kp_co.o rule rather than recompiled here.
KP_AOT_RT := kobold/aot_rt.c kobold/kp_host.c kobold/kp_io.c kobold/str_core.c kobold/str_fmt.c kobold/dynarr.c \
             kobold/map.c kobold/rt_host.c kobold/co_shim.c \
             runtime/co_switch_x86.S jit/call_guest_x86.S
build/libkpaot.a: $(KP_AOT_RT) build/kp_co_lp64.o $(KP_HDRS) Makefile | build
	@rm -rf build/aot && mkdir -p build/aot
	@for f in $(KP_AOT_RT); do \
	    o=build/aot/`basename $$f | sed 's/\.[cS]$$/.o/'`; \
	    echo "  CC $$f"; \
	    $(CC) $(KP_CFLAGS) -c $$f -o $$o || exit 1; \
	done
	@rm -f $@
	$(AR) rcs $@ build/aot/*.o build/kp_co_lp64.o

## skj-kp-rv / skj-kp-arm64: the cross-arch ahead-of-time compilers, the AOT
## counterparts of skj-kp-jit-rv / skj-kp-jit-arm64.  Each is a host binary that
## emits GNU-as assembly for its target through the same selector the matching JIT
## drives, over that target's text sink (rv_mc_text.c / arm64_mc_text.c); the cross
## toolchain then assembles and links it against the target runtime.  aot_main.c is
## arch-neutral; -DKP_AOT_RV only picks the rv entries of its guest-name remap (the
## soft-float real ABI bridge and the stack-ABI 64-bit multiply/shift helpers).
build/skj-kp-rv: kobold/aot_main.c kobold/kpm.c $(IR_SRC) $(BE_RV) kobold/kpm.h ir/ir.h \
                 backend/rv_mc_text.h backend/rv_select.h Makefile | build
	$(CC) $(CFLAGS) -DKP_AOT_RV -DSKJ_KPM_ARCH='"rv32"' -Iir -Ibackend -Ikobold -o $@ \
	    kobold/aot_main.c kobold/kpm.c $(IR_SRC) $(BE_RV)

## AArch64 is LP64, so this AOT compiler is built -DCC_ARM64 (its 8-byte pointer
## width).  arm64_select.c uses a uniform 64-bit integer home (INT_HOME_X:
## ldrx/strx/mov x reloads and call-result captures) unconditionally, since every
## arm64 build is LP64, so a width-8 pointer is never truncated to 32 bits.
build/skj-kp-arm64: kobold/aot_main.c kobold/kpm.c $(IR_SRC) $(BE_A64_SEL) kobold/kpm.h ir/ir.h \
                    backend/arm64_mc_text.h backend/arm64_select.h Makefile | build
	$(CC) $(CFLAGS) -DCC_ARM64 -Iir -Ibackend -Ikobold -o $@ \
	    kobold/aot_main.c kobold/kpm.c $(IR_SRC) $(BE_A64_SEL)

## skj-kp-cf: the ColdFire ahead-of-time compiler for Kobold modules, the AOT
## counterpart over the classic ColdFire backend (BE_CF: regalloc_cf + cf_emit).
## aot_main.c is arch-neutral and takes no -DKP_AOT_RV here (the rv guest-name
## remap is the soft-float bridge that ColdFire does not need).  cf_emit is the
## classic emitter, not a selector, so it ignores aot_main.c's guarded-trap flag:
## Kobold's integer-overflow traps are not enforced on ColdFire for now, which is
## off the kpcom bootstrap critical path.  Output is GAS assembly for skj-as.
build/skj-kp-cf: kobold/aot_main.c kobold/kpm.c $(IR_SRC) $(BE_CF) kobold/kpm.h ir/ir.h Makefile | build
	$(CC) $(CFLAGS) -DSKJ_KPM_ARCH='"cf"' -Iir -Ibackend -Ikobold -o $@ \
	    kobold/aot_main.c kobold/kpm.c $(IR_SRC) $(BE_CF)

## libkpaot-rv.a / libkpaot-arm64.a: the target runtimes an AOT program links
## against, the cross-compiled siblings of libkpaot.a.  Same sources (the ported
## runtime plus the aot_rt.c start file) built with the target toolchain and its
## coroutine switch and call trampoline.  rv also carries jit/rv_jit_i64.S, whose
## stack-ABI 64-bit multiply and shifts the guest reaches through the skj_jit_*
## names (the register-ABI __divdi3 family and all FP math come from the toolchain
## runtime library at link).  arm64 needs neither: i64 and doubles are native.
KP_AOT_RT_RV := kobold/aot_rt.c kobold/kp_host.c kobold/kp_io.c kobold/str_core.c kobold/str_fmt.c kobold/dynarr.c \
                kobold/map.c kobold/rt_host.c kobold/co_shim.c \
                runtime/co_switch_rv.S jit/call_guest_rv.S jit/rv_jit_i64.S \
                kobold/rv_i64_aot_alias.S
build/libkpaot-rv.a: $(KP_AOT_RT_RV) build/kp_co_rv.o $(KP_HDRS_RV) runtime/i64_rv.S Makefile | build
	@rm -rf build/aot-rv && mkdir -p build/aot-rv
	@for f in $(KP_AOT_RT_RV); do \
	    o=build/aot-rv/`basename $$f | sed 's/\.[cS]$$/.o/'`; \
	    echo "  RVCC $$f"; \
	    $(RVJIT_CC) $(KP_CFLAGS_RV) -c $$f -o $$o || exit 1; \
	done
	@rm -f $@
	$(AR) rcs $@ build/aot-rv/*.o build/kp_co_rv.o

KP_AOT_RT_A64 := kobold/aot_rt.c kobold/kp_host.c kobold/kp_io.c kobold/str_core.c kobold/str_fmt.c kobold/dynarr.c \
                 kobold/map.c kobold/rt_host.c kobold/co_shim.c \
                 runtime/co_switch_arm64.S jit/call_guest_arm64.S
build/libkpaot-arm64.a: $(KP_AOT_RT_A64) build/kp_co_a64_lp64.o $(KP_HDRS_A64) Makefile | build
	@rm -rf build/aot-arm64 && mkdir -p build/aot-arm64
	@for f in $(KP_AOT_RT_A64); do \
	    o=build/aot-arm64/`basename $$f | sed 's/\.[cS]$$/.o/'`; \
	    echo "  A64CC $$f"; \
	    $(A64_CC) $(KP_CFLAGS_A64) -DCC_ARM64 -c $$f -o $$o || exit 1; \
	done
	@rm -f $@
	$(AR) rcs $@ build/aot-arm64/*.o build/kp_co_a64_lp64.o

## libkpaot-cf.a: the hosted ColdFire runtime an AOT program links against, the
## big-endian m68k sibling of libkpaot-rv.a / libkpaot-arm64.a.  Same sources (the
## ported runtime plus the aot_rt.c start file) built with the glibc m68k toolchain,
## with the ColdFire coroutine switch (co_switch_cf.S) and a ColdFire call_guest
## trampoline.  ColdFire is natively 32-bit and the toolkit call convention is the
## guest entry's, so the trampoline only switches to the low execution stack and runs
## the parameterless entry.  Unlike the freestanding libkpaot-cf-free.a (a no-libc
## boris tier over skj-run), this links against glibc and runs under qemu-m68k, which
## is what the behavioral corpus (tests/kp-corpus) needs for its emit* externals.
KP_AOT_RT_CF := kobold/aot_rt.c kobold/kp_host.c kobold/kp_io.c kobold/str_core.c kobold/str_fmt.c kobold/dynarr.c \
                kobold/map.c kobold/rt_host.c kobold/co_shim.c \
                runtime/co_switch_cf.S jit/call_guest_cf.S
build/libkpaot-cf.a: $(KP_AOT_RT_CF) build/kp_co_cf.o $(KP_HDRS) Makefile | build
	@rm -rf build/aot-cf && mkdir -p build/aot-cf
	@for f in $(KP_AOT_RT_CF); do \
	    o=build/aot-cf/`basename $$f | sed 's/\.[cS]$$/.o/'`; \
	    echo "  CFCC $$f"; \
	    $(M68K_CC) $(KP_CFLAGS_CF) -c $$f -o $$o || exit 1; \
	done
	@rm -f $@
	$(AR) rcs $@ build/aot-cf/*.o build/kp_co_cf.o

## ---- the bootstrap seed ----
## There is no committed seed.  kpcom is a first-class front end (kobold/kpcom.pas),
## and skj-pc is its bootstrap root: skj-pc compiles kpcom.pas into a per-target
## host front end directly, with no prior Kobold compiler.  Each then emits its own
## source as that target's KPM1 seed at build time: build/kpcom-rv.kpm (from
## skj-kpc-rv, feeds the rv JIT and the rv self-host ELF), build/kpcom-cf.kpm (from
## skj-kpc-cf, feeds the ColdFire self-host ELF), build/kpcom-x86-64.kpm (from
## skj-kpc, feeds the x86-64 JIT), and build/kpcom-arm64.kpm (from skj-kpc-arm64,
## feeds the arm64 JIT).  The in-guest self-host (test-{rv,cf}-selfhost-kpc-full)
## checks that the seed, run on the target, reproduces the host front end's
## reference byte for byte.

## Compiler-independence proof: rebuild kpcom from source with fpc, a toolchain
## independent of both skjegg and a working Kobold, targeting rv32, and check it
## emits the same module as the skj-pc-built rv32 front end.  Since the module
## kpcom emits is fixed by its own logic, not by which compiler built it,
## build/kpcom.kpm.fpc must match build/kpcom-rv.kpm byte for byte.
FPC ?= fpc
bootstrap-fpc: build/kpcom-rv.kpm | build
	@command -v $(FPC) >/dev/null 2>&1 || { echo "bootstrap-fpc: '$(FPC)' required" >&2; exit 1; }
	$(FPC) -dKP_TARGET_RV -Fikobold -FEbuild -FUbuild -O2 -Xs -veiwn kobold/kpcom.pas
	build/kpcom -D KP_TARGET_RV --emit-kpm kobold/kpcom.pas > build/kpcom.kpm.fpc
	@echo "bootstrap-fpc: wrote build/kpcom.kpm.fpc ($$(wc -c < build/kpcom.kpm.fpc | tr -d ' ') bytes, sha256 $$(sha256sum build/kpcom.kpm.fpc | cut -d' ' -f1))"
	@cmp build/kpcom.kpm.fpc build/kpcom-rv.kpm \
	    && echo "bootstrap-fpc: fpc module matches the skj-built rv32 seed (compiler-independent)" \
	    || { echo "bootstrap-fpc: FAIL fpc module differs from build/kpcom-rv.kpm" >&2; exit 1; }

## ---- Kobold source fixtures (tests/kp-src/*.pas + *.expected) ----
## The source-compilation counterpart of check-kp (which exercises the KPM1
## reader on pre-built modules).  run-kp-src-tests.sh runs each fixture in one of
## three modes; see its header.  The x86 targets go in check-all, the rv/arm64
## ones in check-emu-all (they run under qemu).

## Host smoke of the in-process front end: skj-kp-jit --emit-kpm turns each
## fixture into a module whose header is '.kpm 1'.  Proves kp_compile_pas works.
check-kp-frontend: build/skj-kp-jit
	@MODE=frontend FRONTEND=build/skj-kp-jit sh tests/run-kp-src-tests.sh

## Compile and run each fixture in one process with the x86-64 JIT; stdout must
## match the .expected file.
check-kp-jit: build/skj-kp-jit
	@QEMU= MODE=jit JIT=build/skj-kp-jit sh tests/run-kp-src-tests.sh

## Native front end emits a module, the x86-64 backend AOT-compiles and links it,
## the program runs; stdout must match the .expected file.
check-kp-aot: build/skj-kpc build/skj-kp build/libkpaot.a
	@MODE=aot FRONTEND=build/skj-kpc \
	    KPAOT=build/skj-kp KPAOTLIB=build/libkpaot.a \
	    KPAOTCC=$(CC) KPAOTAS="$(X64_ASM) $(X64_ASMFLAGS)" \
	    sh tests/run-kp-src-tests.sh

## Inline assembly in Kobold (asm('text')) is AOT only: the JIT does not
## assemble text.  This runs the asm fixture set through the native front end
## and the x86-64 backend AOT path, the same lane as check-kp-aot.
check-kp-asm: build/skj-kpc build/skj-kp build/libkpaot.a
	@KP_SRCDIR=tests/kp-asm MODE=aot FRONTEND=build/skj-kpc \
	    KPAOT=build/skj-kp KPAOTLIB=build/libkpaot.a \
	    KPAOTCC=$(CC) KPAOTAS="$(X64_ASM) $(X64_ASMFLAGS)" \
	    sh tests/run-kp-src-tests.sh

## The RV32 and AArch64 cross runs, under qemu.  JIT: the embedded front end JITs
## for the cross target in-process.  AOT: the host front end emits the module, the
## cross backend compiles and links it, qemu runs it.
check-kp-jit-rv: build/skj-kp-jit-rv
	@MODE=jit JIT=build/skj-kp-jit-rv QEMU="$(QEMU_RV)" sh tests/run-kp-src-tests.sh

check-kp-jit-arm64: build/skj-kp-jit-arm64
	@MODE=jit JIT=build/skj-kp-jit-arm64 QEMU="$(QEMU_A64)" sh tests/run-kp-src-tests.sh

check-kp-aot-rv: build/skj-kpc-rv build/skj-kp-rv build/libkpaot-rv.a
	@MODE=aot FRONTEND=build/skj-kpc-rv \
	    KPAOT=build/skj-kp-rv KPAOTLIB=build/libkpaot-rv.a \
	    KPAOTCC="$(RVJIT_CC)" KPAOTQEMU="$(QEMU_RV)" \
	    sh tests/run-kp-src-tests.sh

check-kp-aot-arm64: build/skj-kpc-arm64 build/skj-kp-arm64 build/libkpaot-arm64.a
	@MODE=aot FRONTEND=build/skj-kpc-arm64 \
	    KPAOT=build/skj-kp-arm64 KPAOTLIB=build/libkpaot-arm64.a \
	    KPAOTCC="$(A64_CC)" KPAOTQEMU="$(QEMU_A64)" \
	    sh tests/run-kp-src-tests.sh

## ---- the behavioral corpus (tests/kp-corpus) ----
## 63 real Kobold programs vendored from the upstream emit corpus, each with its
## expected stdout generated by the independent reference VM (kprun), covering the
## language's behavior: arithmetic, records, arrays, maps, dynamic arrays, strings,
## enums, sets, coroutines, reals and transcendentals, int64, casts, var
## parameters, and control flow.  Every program runs through the hosted tiers and
## must produce byte-identical stdout, so one .expected pins the result across the
## x86-64 JIT and the x86-64/rv32/arm64 AOT paths at once (the cross-backend
## equivalence the external runcheck checks, now in-tree).  The input-dependent and
## include-file cases stay external; the failable two-return (CALL2) cases
## failforms/failmethod are included (their dst=-1 discard needed the reader fix).
check-kp-corpus: build/skj-kp-jit build/skj-kpc build/skj-kp build/libkpaot.a
	@QEMU= KP_SRCDIR=tests/kp-corpus MODE=jit JIT=build/skj-kp-jit sh tests/run-kp-src-tests.sh
	@KP_SRCDIR=tests/kp-corpus MODE=aot FRONTEND=build/skj-kpc \
	    KPAOT=build/skj-kp KPAOTLIB=build/libkpaot.a \
	    KPAOTCC=$(CC) KPAOTAS="$(X64_ASM) $(X64_ASMFLAGS)" \
	    sh tests/run-kp-src-tests.sh

## check-kp-corpus-san: run the whole behavioral corpus through the instrumented JIT
## (GH_POISON + ASan/UBSan) and fail on any report.  This is the memory-safety gate for
## the managed runtimes' reference counting, the check a value-only corpus run cannot make:
## a string/array mutator that over-releases, double-frees, or reads freed storage prints
## the right answer yet trips here.  Run it after any change to the string, dynamic-array,
## or map runtime (the string self-append path is what it was added for).
check-kp-corpus-san: build/skj-kp-jit-san
	@env $(SAN_RUNENV) sh -c 'hits=0; \
	  for f in tests/kp-corpus/*.pas; do \
	    out=$$(build/skj-kp-jit-san --run "$$f" 2>&1); \
	    if echo "$$out" | grep -qiE "AddressSanitizer|runtime error|use-after-free|write-after-free|GH_"; then \
	      echo "FAIL $$f"; echo "$$out" | tail -8; hits=$$((hits + 1)); \
	    fi; \
	  done; \
	  if [ $$hits -ne 0 ]; then echo "check-kp-corpus-san: $$hits file(s) tripped the instrument" >&2; exit 1; fi; \
	  echo "check-kp-corpus-san: corpus clean under GH_POISON + ASan/UBSan"'

## The cross-arch corpus runs the AOT path only: the host front end and backend do
## the work, qemu runs the small program (a cross JIT would emulate JITing the
## whole seed, far too slow for the whole corpus).
check-kp-corpus-rv: build/skj-kpc-rv build/skj-kp-rv build/libkpaot-rv.a
	@KP_SRCDIR=tests/kp-corpus MODE=aot FRONTEND=build/skj-kpc-rv \
	    KPAOT=build/skj-kp-rv KPAOTLIB=build/libkpaot-rv.a \
	    KPAOTCC="$(RVJIT_CC)" KPAOTQEMU="$(QEMU_RV)" \
	    sh tests/run-kp-src-tests.sh

check-kp-corpus-arm64: build/skj-kpc-arm64 build/skj-kp-arm64 build/libkpaot-arm64.a
	@KP_SRCDIR=tests/kp-corpus MODE=aot FRONTEND=build/skj-kpc-arm64 \
	    KPAOT=build/skj-kp-arm64 KPAOTLIB=build/libkpaot-arm64.a \
	    KPAOTCC="$(A64_CC)" KPAOTQEMU="$(QEMU_A64)" \
	    sh tests/run-kp-src-tests.sh

## The ColdFire corpus runs the hosted AOT path under qemu-m68k: skj-kp-cf emits
## big-endian m68k asm from the agnostic seed, the glibc m68k gcc assembles and
## links it against libkpaot-cf.a, and qemu-m68k runs the program.  This is the
## big-endian cross-check of the whole corpus, the hosted sibling of the
## freestanding check-kp-free-cf (which runs the smaller kp-src set on skj-run).
## Linked -static: the glibc m68k driver would otherwise produce a dynamic ELF
## needing /lib/ld.so.1, which qemu-m68k has no sysroot for (every other
## qemu-m68k suite here is static/freestanding).  A static link needs no loader.
check-kp-corpus-cf: build/skj-kpc-cf build/skj-kp-cf build/libkpaot-cf.a
	@KP_SRCDIR=tests/kp-corpus MODE=aot FRONTEND=build/skj-kpc-cf \
	    KPAOT=build/skj-kp-cf KPAOTLIB=build/libkpaot-cf.a \
	    KPAOTCC="$(M68K_CC) -static" KPAOTQEMU="$(QEMU)" \
	    sh tests/run-kp-src-tests.sh

## ---- the multi-unit corpus (tests/kp-multi) ----
## Programs that `uses` units, the one behavioral surface the single-module corpus
## cannot reach: cross-unit calls, a unit const, a unit's mutable global and shared
## var, an exported enum and record type, qualified `as`, and a transitive `uses`
## chain.  run-kp-multi-tests.sh compiles each unit to its own module first, then
## runs the programs against the set; the .expected stdout is the independent
## reference kprun's, like the single-module corpus.  The JIT links the modules in
## memory, the AOT path links their objects (aot-run.sh).  x86 runs the JIT and
## AOT (in check-all); rv32 and arm64 run the AOT path under qemu (in
## check-emu-all), so one .expected pins every backend and the in-memory and
## system links agree.
check-kp-multi: build/skj-kp-jit build/skj-kpc build/skj-kp build/libkpaot.a
	@QEMU= KP_SRCDIR=tests/kp-multi MODE=jit JIT=build/skj-kp-jit sh tests/run-kp-multi-tests.sh
	@KP_SRCDIR=tests/kp-multi MODE=aot FRONTEND=build/skj-kpc \
	    KPAOT=build/skj-kp KPAOTLIB=build/libkpaot.a \
	    KPAOTCC=$(CC) KPAOTAS="$(X64_ASM) $(X64_ASMFLAGS)" \
	    sh tests/run-kp-multi-tests.sh

check-kp-multi-rv: build/skj-kpc-rv build/skj-kp-rv build/libkpaot-rv.a
	@KP_SRCDIR=tests/kp-multi MODE=aot FRONTEND=build/skj-kpc-rv \
	    KPAOT=build/skj-kp-rv KPAOTLIB=build/libkpaot-rv.a \
	    KPAOTCC="$(RVJIT_CC)" KPAOTQEMU="$(QEMU_RV)" \
	    sh tests/run-kp-multi-tests.sh

check-kp-multi-arm64: build/skj-kpc-arm64 build/skj-kp-arm64 build/libkpaot-arm64.a
	@KP_SRCDIR=tests/kp-multi MODE=aot FRONTEND=build/skj-kpc-arm64 \
	    KPAOT=build/skj-kp-arm64 KPAOTLIB=build/libkpaot-arm64.a \
	    KPAOTCC="$(A64_CC)" KPAOTQEMU="$(QEMU_A64)" \
	    sh tests/run-kp-multi-tests.sh

## ---- file I/O provided as a Kobold unit (tests/kp-files) ----
## The `files` library unit (kobold/lib/files.pas) gives a program whole-file
## read/write through `uses files`, instead of a per-program external declaration
## of the runtime's ReadFileStr/WriteFileStr primitives.  The unit is precompiled
## to an interface+code .kpm, staged beside a copy of the program (the compiler
## resolves a used unit's interface in the program's own directory; the kp-multi
## harness co-locates the same way).  A program that `uses files` then round-trips
## a file through it on the x86-64 JIT and AOT paths.  The temp path is handed in
## as an argument so the test writes only under build/.
build/kp-files/io/files.kpm: kobold/lib/io/files.pas build/skj-kpc | build
	@mkdir -p build/kp-files/io
	./build/skj-kpc --emit-kpm $< > $@

check-kp-files: build/skj-kp-jit build/skj-kpc build/skj-kpc-rv build/skj-kp build/libkpaot.a \
                build/skj-kp-rv build/skj-as-rv build/skj-ld-rv \
                build/kpfree-rv/kp_crt_rv.o build/libkpaot-rv-free.a \
                build/kp-files/io/files.kpm
	@cp tests/kp-files/filesrt.pas build/kp-files/filesrt.pas
	@rm -f build/kp-files/rt.txt
	@./build/skj-kp-jit build/kp-files/filesrt.pas $(CURDIR)/build/kp-files/rt.txt \
	    > build/kp-files/jit.out 2>build/kp-files/jit.err; \
	 cmp -s build/kp-files/jit.out tests/kp-files/filesrt.expected \
	    || { echo "FAIL kp-files (jit)"; cat build/kp-files/jit.err build/kp-files/jit.out; exit 1; }
	@./build/skj-kpc --emit-kpm build/kp-files/filesrt.pas \
	    > build/kp-files/filesrt.kpm 2>build/kp-files/emit.err
	@rm -f build/kp-files/rt.txt
	@KOBOLD_UNITS=$(CURDIR)/build/kp-files KPAOT=build/skj-kp KPAOTLIB=build/libkpaot.a \
	    KPAOTCC=$(CC) KPAOTAS="$(X64_ASM) $(X64_ASMFLAGS)" \
	    sh kobold/aot-run.sh build/kp-files/filesrt.kpm $(CURDIR)/build/kp-files/rt.txt \
	    > build/kp-files/aot.out 2>build/kp-files/aot.err; \
	 cmp -s build/kp-files/aot.out tests/kp-files/filesrt.expected \
	    || { echo "FAIL kp-files (aot)"; cat build/kp-files/aot.err build/kp-files/aot.out; exit 1; }
	@# freestanding rv32: the free runner does not walk `.uses`, so compile the
	@# unit and the program for rv32 and link them with the crt + free archive.
	@# rv32 is ILP32, so re-emit both modules width-4 with skj-kpc-rv (the x86-64
	@# AOT path above uses the width-8 skj-kpc modules).
	@./build/skj-kpc-rv --emit-kpm kobold/lib/io/files.pas > build/kp-files/io/files-32.kpm 2>build/kp-files/rv.err
	@./build/skj-kpc-rv --emit-kpm build/kp-files/filesrt.pas > build/kp-files/filesrt-32.kpm 2>>build/kp-files/rv.err
	@./build/skj-kp-rv build/kp-files/io/files-32.kpm -o build/kp-files/files_rv.s 2>>build/kp-files/rv.err
	@./build/skj-as-rv -o build/kp-files/files_rv.o build/kp-files/files_rv.s 2>>build/kp-files/rv.err
	@./build/skj-kp-rv build/kp-files/filesrt-32.kpm -o build/kp-files/filesrt_rv.s 2>>build/kp-files/rv.err
	@./build/skj-as-rv -o build/kp-files/filesrt_rv.o build/kp-files/filesrt_rv.s 2>>build/kp-files/rv.err
	@./build/skj-ld-rv -o build/kp-files/filesrt_rv.elf build/kpfree-rv/kp_crt_rv.o \
	    build/kp-files/filesrt_rv.o build/kp-files/files_rv.o build/libkpaot-rv-free.a 2>>build/kp-files/rv.err
	@rm -f build/kp-files/rt.txt
	@$(QEMU_RV) build/kp-files/filesrt_rv.elf $(CURDIR)/build/kp-files/rt.txt \
	    > build/kp-files/free-rv.out 2>build/kp-files/free-rv.err; \
	 cmp -s build/kp-files/free-rv.out tests/kp-files/filesrt.expected \
	    || { echo "FAIL kp-files (free-rv)"; cat build/kp-files/rv.err build/kp-files/free-rv.err build/kp-files/free-rv.out; exit 1; }
	@echo "kp-files: 1 passed (jit, aot, free-rv)"

build/kp-dirs/io/dirs.kpm: kobold/lib/io/dirs.pas build/skj-kpc | build
	@mkdir -p build/kp-dirs/io
	./build/skj-kpc --emit-kpm $< > $@

## Directory reads reached through the `dirs` unit, over the same three tiers as
## check-kp-files.  A scratch directory of three known files is rebuilt before
## each runner; the program reports an order-independent entry count and total
## name length, so the expected output is deterministic.
check-kp-dirs: build/skj-kp-jit build/skj-kpc build/skj-kpc-rv build/skj-kp build/libkpaot.a \
               build/skj-kp-rv build/skj-as-rv build/skj-ld-rv \
               build/kpfree-rv/kp_crt_rv.o build/libkpaot-rv-free.a \
               build/kp-dirs/io/dirs.kpm
	@cp tests/kp-dirs/dirsrt.pas build/kp-dirs/dirsrt.pas
	@mkscan() { rm -rf "$$1"; mkdir -p "$$1"; : > "$$1/alpha"; : > "$$1/beta"; : > "$$1/gamma"; }; \
	 D=$(CURDIR)/build/kp-dirs/scan; \
	 mkscan $$D; \
	 ./build/skj-kp-jit build/kp-dirs/dirsrt.pas $$D \
	     > build/kp-dirs/jit.out 2>build/kp-dirs/jit.err; \
	 cmp -s build/kp-dirs/jit.out tests/kp-dirs/dirsrt.expected \
	     || { echo "FAIL kp-dirs (jit)"; cat build/kp-dirs/jit.err build/kp-dirs/jit.out; exit 1; }; \
	 ./build/skj-kpc --emit-kpm build/kp-dirs/dirsrt.pas \
	     > build/kp-dirs/dirsrt.kpm 2>build/kp-dirs/emit.err; \
	 mkscan $$D; \
	 KOBOLD_UNITS=$(CURDIR)/build/kp-dirs KPAOT=build/skj-kp KPAOTLIB=build/libkpaot.a \
	     KPAOTCC=$(CC) KPAOTAS="$(X64_ASM) $(X64_ASMFLAGS)" \
	     sh kobold/aot-run.sh build/kp-dirs/dirsrt.kpm $$D \
	     > build/kp-dirs/aot.out 2>build/kp-dirs/aot.err; \
	 cmp -s build/kp-dirs/aot.out tests/kp-dirs/dirsrt.expected \
	     || { echo "FAIL kp-dirs (aot)"; cat build/kp-dirs/aot.err build/kp-dirs/aot.out; exit 1; }; \
	 ./build/skj-kpc-rv --emit-kpm kobold/lib/io/dirs.pas > build/kp-dirs/io/dirs-32.kpm 2>build/kp-dirs/rv.err; \
	 ./build/skj-kpc-rv --emit-kpm build/kp-dirs/dirsrt.pas > build/kp-dirs/dirsrt-32.kpm 2>>build/kp-dirs/rv.err; \
	 ./build/skj-kp-rv build/kp-dirs/io/dirs-32.kpm -o build/kp-dirs/dirs_rv.s 2>>build/kp-dirs/rv.err; \
	 ./build/skj-as-rv -o build/kp-dirs/dirs_rv.o build/kp-dirs/dirs_rv.s 2>>build/kp-dirs/rv.err; \
	 ./build/skj-kp-rv build/kp-dirs/dirsrt-32.kpm -o build/kp-dirs/dirsrt_rv.s 2>>build/kp-dirs/rv.err; \
	 ./build/skj-as-rv -o build/kp-dirs/dirsrt_rv.o build/kp-dirs/dirsrt_rv.s 2>>build/kp-dirs/rv.err; \
	 ./build/skj-ld-rv -o build/kp-dirs/dirsrt_rv.elf build/kpfree-rv/kp_crt_rv.o \
	     build/kp-dirs/dirsrt_rv.o build/kp-dirs/dirs_rv.o build/libkpaot-rv-free.a 2>>build/kp-dirs/rv.err; \
	 mkscan $$D; \
	 $(QEMU_RV) build/kp-dirs/dirsrt_rv.elf $$D \
	     > build/kp-dirs/free-rv.out 2>build/kp-dirs/free-rv.err; \
	 cmp -s build/kp-dirs/free-rv.out tests/kp-dirs/dirsrt.expected \
	     || { echo "FAIL kp-dirs (free-rv)"; cat build/kp-dirs/rv.err build/kp-dirs/free-rv.err build/kp-dirs/free-rv.out; exit 1; }
	@echo "kp-dirs: 1 passed (jit, aot, free-rv)"

## ---- the std/io stdlib wrapper units (tests/kp-stdlib) ----
## The runtime glue for random, time, and the console latch (runtime/rng_glue.c,
## sched_glue.c, kobold/io_free.c) is presented as units a program `uses`:
## std.random, std.time, io.console.  The glue ships in the rv freestanding
## archive, so the units are exercised on that tier (boris's Kobold target): each
## unit and the program compile to rv objects and link with the crt and the free
## archive.  The dotted unit names resolve through the front end's own directory
## (dot -> path separator), so the unit .kpm files are staged under io/ and std/.
build/kp-stdlib/io/console.kpm: kobold/lib/io/console.pas build/skj-kpc-rv | build
	@mkdir -p build/kp-stdlib/io
	./build/skj-kpc-rv --emit-kpm $< > $@
build/kp-stdlib/io/buffered.kpm: kobold/lib/io/buffered.pas build/skj-kpc-rv | build
	@mkdir -p build/kp-stdlib/io
	./build/skj-kpc-rv --emit-kpm $< > $@
build/kp-stdlib/std/time.kpm: kobold/lib/std/time.pas build/skj-kpc-rv | build
	@mkdir -p build/kp-stdlib/std
	./build/skj-kpc-rv --emit-kpm $< > $@
build/kp-stdlib/std/random.kpm: kobold/lib/std/random.pas build/skj-kpc-rv | build
	@mkdir -p build/kp-stdlib/std
	./build/skj-kpc-rv --emit-kpm $< > $@
build/kp-stdlib/core/strings.kpm: kobold/lib/core/strings.pas build/skj-kpc-rv | build
	@mkdir -p build/kp-stdlib/core
	./build/skj-kpc-rv --emit-kpm $< > $@
build/kp-stdlib/core/convert.kpm: kobold/lib/core/convert.pas build/skj-kpc-rv | build
	@mkdir -p build/kp-stdlib/core
	./build/skj-kpc-rv --emit-kpm $< > $@

## The ColdFire sibling units: the same sources emitted by the ColdFire front end,
## so they carry `.target cf` and the ColdFire backend accepts them.  A unit is
## bound to its target, so cf gets its own copies rather than reusing the rv ones.
build/kp-stdlib-cf/io/console.kpm: kobold/lib/io/console.pas build/skj-kpc-cf | build
	@mkdir -p build/kp-stdlib-cf/io
	./build/skj-kpc-cf --emit-kpm $< > $@
build/kp-stdlib-cf/std/time.kpm: kobold/lib/std/time.pas build/skj-kpc-cf | build
	@mkdir -p build/kp-stdlib-cf/std
	./build/skj-kpc-cf --emit-kpm $< > $@
build/kp-stdlib-cf/std/random.kpm: kobold/lib/std/random.pas build/skj-kpc-cf | build
	@mkdir -p build/kp-stdlib-cf/std
	./build/skj-kpc-cf --emit-kpm $< > $@
build/kp-stdlib-cf/core/convert.kpm: kobold/lib/core/convert.pas build/skj-kpc-cf | build
	@mkdir -p build/kp-stdlib-cf/core
	./build/skj-kpc-cf --emit-kpm $< > $@

check-kp-stdlib-free: build/skj-kpc-rv build/skj-kp-rv build/skj-as-rv build/skj-ld-rv \
                      build/kpfree-rv/kp_crt_rv.o build/libkpaot-rv-free.a \
                      build/kp-stdlib/io/console.kpm build/kp-stdlib/io/buffered.kpm \
                      build/kp-stdlib/std/time.kpm \
                      build/kp-stdlib/std/random.kpm build/kp-stdlib/core/strings.kpm \
                      build/kp-stdlib/core/convert.kpm
	@cp tests/kp-stdlib/stddemo.pas build/kp-stdlib/stddemo.pas
	@E=build/kp-stdlib/build.err; : > $$E; \
	 ./build/skj-kpc-rv --emit-kpm build/kp-stdlib/stddemo.pas > build/kp-stdlib/stddemo.kpm 2>>$$E; \
	 for u in io/console std/time std/random stddemo; do \
	   b=$$(basename $$u); \
	   ./build/skj-kp-rv build/kp-stdlib/$$u.kpm -o build/kp-stdlib/$$b.s 2>>$$E && \
	   ./build/skj-as-rv -o build/kp-stdlib/$$b.o build/kp-stdlib/$$b.s 2>>$$E || \
	     { echo "FAIL kp-stdlib (compile $$u)"; cat $$E; exit 1; }; \
	 done; \
	 ./build/skj-ld-rv -o build/kp-stdlib/stddemo.elf build/kpfree-rv/kp_crt_rv.o \
	   build/kp-stdlib/stddemo.o build/kp-stdlib/console.o build/kp-stdlib/time.o \
	   build/kp-stdlib/random.o build/libkpaot-rv-free.a 2>>$$E || \
	     { echo "FAIL kp-stdlib (link)"; cat $$E; exit 1; }; \
	 $(QEMU_RV) build/kp-stdlib/stddemo.elf </dev/null > build/kp-stdlib/out 2>>$$E; \
	 cmp -s build/kp-stdlib/out tests/kp-stdlib/stddemo.expected \
	   || { echo "FAIL kp-stdlib (free-rv)"; cat $$E build/kp-stdlib/out; exit 1; }
	@cp tests/kp-stdlib/consoleio.pas build/kp-stdlib/consoleio.pas
	@E=build/kp-stdlib/build.err; : > $$E; \
	 ./build/skj-kpc-rv --emit-kpm build/kp-stdlib/consoleio.pas > build/kp-stdlib/consoleio.kpm 2>>$$E; \
	 ./build/skj-kp-rv build/kp-stdlib/consoleio.kpm -o build/kp-stdlib/consoleio.s 2>>$$E && \
	 ./build/skj-as-rv -o build/kp-stdlib/consoleio.o build/kp-stdlib/consoleio.s 2>>$$E || \
	   { echo "FAIL kp-stdlib (compile consoleio)"; cat $$E; exit 1; }; \
	 ./build/skj-ld-rv -o build/kp-stdlib/consoleio.elf build/kpfree-rv/kp_crt_rv.o \
	   build/kp-stdlib/consoleio.o build/kp-stdlib/console.o build/libkpaot-rv-free.a 2>>$$E || \
	     { echo "FAIL kp-stdlib (link consoleio)"; cat $$E; exit 1; }; \
	 $(QEMU_RV) build/kp-stdlib/consoleio.elf < tests/kp-stdlib/consoleio.input \
	   > build/kp-stdlib/conout 2>>$$E; \
	 cmp -s build/kp-stdlib/conout tests/kp-stdlib/consoleio.expected \
	   || { echo "FAIL kp-stdlib (consoleio)"; cat $$E build/kp-stdlib/conout; exit 1; }
	@cp tests/kp-stdlib/bufferedio.pas build/kp-stdlib/bufferedio.pas
	@E=build/kp-stdlib/build.err; : > $$E; \
	 ./build/skj-kpc-rv --emit-kpm build/kp-stdlib/bufferedio.pas > build/kp-stdlib/bufferedio.kpm 2>>$$E; \
	 ./build/skj-kp-rv build/kp-stdlib/io/buffered.kpm -o build/kp-stdlib/buffered.s 2>>$$E && \
	 ./build/skj-as-rv -o build/kp-stdlib/buffered.o build/kp-stdlib/buffered.s 2>>$$E || \
	   { echo "FAIL kp-stdlib (compile io.buffered)"; cat $$E; exit 1; }; \
	 ./build/skj-kp-rv build/kp-stdlib/bufferedio.kpm -o build/kp-stdlib/bufferedio.s 2>>$$E && \
	 ./build/skj-as-rv -o build/kp-stdlib/bufferedio.o build/kp-stdlib/bufferedio.s 2>>$$E || \
	   { echo "FAIL kp-stdlib (compile bufferedio)"; cat $$E; exit 1; }; \
	 ./build/skj-ld-rv -o build/kp-stdlib/bufferedio.elf build/kpfree-rv/kp_crt_rv.o \
	   build/kp-stdlib/bufferedio.o build/kp-stdlib/buffered.o build/libkpaot-rv-free.a 2>>$$E || \
	     { echo "FAIL kp-stdlib (link bufferedio)"; cat $$E; exit 1; }; \
	 $(QEMU_RV) build/kp-stdlib/bufferedio.elf < tests/kp-stdlib/bufferedio.input \
	   > build/kp-stdlib/bufout 2>>$$E; \
	 cmp -s build/kp-stdlib/bufout tests/kp-stdlib/bufferedio.expected \
	   || { echo "FAIL kp-stdlib (bufferedio)"; cat $$E build/kp-stdlib/bufout; exit 1; }
	@cp tests/kp-stdlib/realunit.pas build/kp-stdlib/realunit.pas
	@E=build/kp-stdlib/build.err; : > $$E; \
	 ./build/skj-kpc-rv --emit-kpm build/kp-stdlib/realunit.pas > build/kp-stdlib/realunit.kpm 2>>$$E; \
	 ./build/skj-kp-rv build/kp-stdlib/realunit.kpm -o build/kp-stdlib/realunit.s 2>>$$E && \
	 ./build/skj-as-rv -o build/kp-stdlib/realunit.o build/kp-stdlib/realunit.s 2>>$$E || \
	   { echo "FAIL kp-stdlib (compile realunit)"; cat $$E; exit 1; }; \
	 ./build/skj-ld-rv -o build/kp-stdlib/realunit.elf build/kpfree-rv/kp_crt_rv.o \
	   build/kp-stdlib/realunit.o build/kp-stdlib/console.o build/kp-stdlib/random.o \
	   build/libkpaot-rv-free.a 2>>$$E || \
	     { echo "FAIL kp-stdlib (link realunit)"; cat $$E; exit 1; }; \
	 $(QEMU_RV) build/kp-stdlib/realunit.elf < tests/kp-stdlib/realunit.input \
	   > build/kp-stdlib/realout 2>>$$E; \
	 cmp -s build/kp-stdlib/realout tests/kp-stdlib/realunit.expected \
	   || { echo "FAIL kp-stdlib (realunit)"; cat $$E build/kp-stdlib/realout; exit 1; }
	@cp tests/kp-stdlib/nswild.pas build/kp-stdlib/nswild.pas
	@E=build/kp-stdlib/build.err; : > $$E; \
	 ./build/skj-kpc-rv --emit-kpm build/kp-stdlib/nswild.pas > build/kp-stdlib/nswild.kpm 2>>$$E || \
	   { echo "FAIL kp-stdlib (compile nswild: uses ns.*)"; cat $$E; exit 1; }; \
	 ./build/skj-kp-rv build/kp-stdlib/nswild.kpm -o build/kp-stdlib/nswild.s 2>>$$E && \
	 ./build/skj-as-rv -o build/kp-stdlib/nswild.o build/kp-stdlib/nswild.s 2>>$$E || \
	   { echo "FAIL kp-stdlib (assemble nswild)"; cat $$E; exit 1; }; \
	 ./build/skj-ld-rv -o build/kp-stdlib/nswild.elf build/kpfree-rv/kp_crt_rv.o \
	   build/kp-stdlib/nswild.o build/kp-stdlib/console.o build/kp-stdlib/time.o \
	   build/kp-stdlib/random.o build/libkpaot-rv-free.a 2>>$$E || \
	     { echo "FAIL kp-stdlib (link nswild)"; cat $$E; exit 1; }; \
	 $(QEMU_RV) build/kp-stdlib/nswild.elf </dev/null > build/kp-stdlib/wildout 2>>$$E; \
	 cmp -s build/kp-stdlib/wildout tests/kp-stdlib/nswild.expected \
	   || { echo "FAIL kp-stdlib (nswild)"; cat $$E build/kp-stdlib/wildout; exit 1; }
	@cp tests/kp-stdlib/strdemo.pas build/kp-stdlib/strdemo.pas
	@E=build/kp-stdlib/build.err; : > $$E; \
	 ./build/skj-kpc-rv --emit-kpm build/kp-stdlib/strdemo.pas > build/kp-stdlib/strdemo.kpm 2>>$$E || \
	   { echo "FAIL kp-stdlib (compile strdemo: core.strings)"; cat $$E; exit 1; }; \
	 ./build/skj-kp-rv build/kp-stdlib/strdemo.kpm -o build/kp-stdlib/strdemo.s 2>>$$E && \
	 ./build/skj-as-rv -o build/kp-stdlib/strdemo.o build/kp-stdlib/strdemo.s 2>>$$E || \
	   { echo "FAIL kp-stdlib (assemble strdemo)"; cat $$E; exit 1; }; \
	 ./build/skj-kp-rv build/kp-stdlib/core/strings.kpm -o build/kp-stdlib/strings.s 2>>$$E && \
	 ./build/skj-as-rv -o build/kp-stdlib/strings.o build/kp-stdlib/strings.s 2>>$$E || \
	   { echo "FAIL kp-stdlib (assemble core.strings)"; cat $$E; exit 1; }; \
	 ./build/skj-ld-rv -o build/kp-stdlib/strdemo.elf build/kpfree-rv/kp_crt_rv.o \
	   build/kp-stdlib/strdemo.o build/kp-stdlib/strings.o build/libkpaot-rv-free.a 2>>$$E || \
	     { echo "FAIL kp-stdlib (link strdemo)"; cat $$E; exit 1; }; \
	 $(QEMU_RV) build/kp-stdlib/strdemo.elf </dev/null > build/kp-stdlib/strout 2>>$$E; \
	 cmp -s build/kp-stdlib/strout tests/kp-stdlib/strdemo.expected \
	   || { echo "FAIL kp-stdlib (strdemo)"; cat $$E build/kp-stdlib/strout; exit 1; }
	@cp tests/kp-stdlib/convertnum.pas build/kp-stdlib/convertnum.pas
	@E=build/kp-stdlib/build.err; : > $$E; \
	 ./build/skj-kpc-rv --emit-kpm build/kp-stdlib/convertnum.pas > build/kp-stdlib/convertnum.kpm 2>>$$E || \
	   { echo "FAIL kp-stdlib (compile convertnum: core.convert)"; cat $$E; exit 1; }; \
	 ./build/skj-kp-rv build/kp-stdlib/convertnum.kpm -o build/kp-stdlib/convertnum.s 2>>$$E && \
	 ./build/skj-as-rv -o build/kp-stdlib/convertnum.o build/kp-stdlib/convertnum.s 2>>$$E || \
	   { echo "FAIL kp-stdlib (assemble convertnum)"; cat $$E; exit 1; }; \
	 ./build/skj-kp-rv build/kp-stdlib/core/convert.kpm -o build/kp-stdlib/convert.s 2>>$$E && \
	 ./build/skj-as-rv -o build/kp-stdlib/convert.o build/kp-stdlib/convert.s 2>>$$E || \
	   { echo "FAIL kp-stdlib (assemble core.convert)"; cat $$E; exit 1; }; \
	 ./build/skj-ld-rv -o build/kp-stdlib/convertnum.elf build/kpfree-rv/kp_crt_rv.o \
	   build/kp-stdlib/convertnum.o build/kp-stdlib/convert.o build/kp-stdlib/console.o \
	   build/libkpaot-rv-free.a 2>>$$E || \
	     { echo "FAIL kp-stdlib (link convertnum)"; cat $$E; exit 1; }; \
	 $(QEMU_RV) build/kp-stdlib/convertnum.elf </dev/null > build/kp-stdlib/convout 2>>$$E; \
	 cmp -s build/kp-stdlib/convout tests/kp-stdlib/convertnum.expected \
	   || { echo "FAIL kp-stdlib (convertnum)"; cat $$E build/kp-stdlib/convout; exit 1; }
	@echo "kp-stdlib: 7 passed (free-rv): std.random (+ real/secure), std.time, io.console (+ read/write, real), io.buffered, realunit, uses ns.*, core.strings, core.convert"

## The ColdFire sibling of check-kp-stdlib-free: the same stddemo (std.random,
## std.time, io.console) over the cf freestanding tier under skj-run, proving the
## ColdFire time/rng/yield syscall wrappers (syscall_cf.S) and the emulator's
## GSYS_CLOCK_GETTIME / GSYS_GETRANDOM / GSYS_NANOSLEEP maps (emu/cf_user.c).  The
## .kpm modules are arch-neutral (host skj-kpc), so only the backend and the link
## differ from the rv target.  The unit .kpm modules are cf-specific (build/
## kp-stdlib-cf, emitted by the ColdFire front end), since a unit is bound to its
## target; outputs go there too so the rv objects are not clobbered.
check-kp-stdlib-free-cf: build/skj-kpc-cf build/skj-kp-cf build/skj-as build/skj-ld \
                         build/skj-run build/kpfree-cf/kp_crt_cf.o build/libkpaot-cf-free.a \
                         build/kp-stdlib-cf/io/console.kpm build/kp-stdlib-cf/std/time.kpm \
                         build/kp-stdlib-cf/std/random.kpm build/kp-stdlib-cf/core/convert.kpm
	@mkdir -p build/kp-stdlib-cf
	@cp tests/kp-stdlib/stddemo.pas build/kp-stdlib-cf/stddemo.pas
	@E=build/kp-stdlib-cf/build.err; : > $$E; \
	 ./build/skj-kpc-cf -U build/kp-stdlib-cf --emit-kpm build/kp-stdlib-cf/stddemo.pas > build/kp-stdlib-cf/stddemo.kpm 2>>$$E; \
	 for u in io/console std/time std/random; do \
	   b=$$(basename $$u); \
	   ./build/skj-kp-cf build/kp-stdlib-cf/$$u.kpm -o build/kp-stdlib-cf/$$b.s 2>>$$E && \
	   ./build/skj-as -o build/kp-stdlib-cf/$$b.o build/kp-stdlib-cf/$$b.s 2>>$$E || \
	     { echo "FAIL kp-stdlib-cf (compile $$u)"; cat $$E; exit 1; }; \
	 done; \
	 ./build/skj-kp-cf build/kp-stdlib-cf/stddemo.kpm -o build/kp-stdlib-cf/stddemo.s 2>>$$E && \
	 ./build/skj-as -o build/kp-stdlib-cf/stddemo.o build/kp-stdlib-cf/stddemo.s 2>>$$E || \
	   { echo "FAIL kp-stdlib-cf (compile stddemo)"; cat $$E; exit 1; }; \
	 ./build/skj-ld -T kobold/kp_cf.ld -o build/kp-stdlib-cf/stddemo.elf build/kpfree-cf/kp_crt_cf.o \
	   build/kp-stdlib-cf/stddemo.o build/kp-stdlib-cf/console.o build/kp-stdlib-cf/time.o \
	   build/kp-stdlib-cf/random.o build/libkpaot-cf-free.a 2>>$$E || \
	     { echo "FAIL kp-stdlib-cf (link)"; cat $$E; exit 1; }; \
	 build/skj-run build/kp-stdlib-cf/stddemo.elf </dev/null > build/kp-stdlib-cf/out 2>>$$E; \
	 cmp -s build/kp-stdlib-cf/out tests/kp-stdlib/stddemo.expected \
	   || { echo "FAIL kp-stdlib-cf (free-cf)"; cat $$E build/kp-stdlib-cf/out; exit 1; }
	@cp tests/kp-stdlib/convertnum.pas build/kp-stdlib-cf/convertnum.pas
	@E=build/kp-stdlib-cf/build.err; : > $$E; \
	 ./build/skj-kpc-cf -U build/kp-stdlib-cf --emit-kpm build/kp-stdlib-cf/convertnum.pas > build/kp-stdlib-cf/convertnum.kpm 2>>$$E || \
	   { echo "FAIL kp-stdlib-cf (compile convertnum)"; cat $$E; exit 1; }; \
	 ./build/skj-kp-cf build/kp-stdlib-cf/core/convert.kpm -o build/kp-stdlib-cf/convert.s 2>>$$E && \
	 ./build/skj-as -o build/kp-stdlib-cf/convert.o build/kp-stdlib-cf/convert.s 2>>$$E || \
	   { echo "FAIL kp-stdlib-cf (assemble core.convert)"; cat $$E; exit 1; }; \
	 ./build/skj-kp-cf build/kp-stdlib-cf/convertnum.kpm -o build/kp-stdlib-cf/convertnum.s 2>>$$E && \
	 ./build/skj-as -o build/kp-stdlib-cf/convertnum.o build/kp-stdlib-cf/convertnum.s 2>>$$E || \
	   { echo "FAIL kp-stdlib-cf (assemble convertnum)"; cat $$E; exit 1; }; \
	 ./build/skj-ld -T kobold/kp_cf.ld -o build/kp-stdlib-cf/convertnum.elf build/kpfree-cf/kp_crt_cf.o \
	   build/kp-stdlib-cf/convertnum.o build/kp-stdlib-cf/convert.o build/kp-stdlib-cf/console.o \
	   build/libkpaot-cf-free.a 2>>$$E || \
	     { echo "FAIL kp-stdlib-cf (link convertnum)"; cat $$E; exit 1; }; \
	 build/skj-run build/kp-stdlib-cf/convertnum.elf </dev/null > build/kp-stdlib-cf/convout 2>>$$E; \
	 cmp -s build/kp-stdlib-cf/convout tests/kp-stdlib/convertnum.expected \
	   || { echo "FAIL kp-stdlib-cf (convertnum)"; cat $$E build/kp-stdlib-cf/convout; exit 1; }
	@echo "kp-stdlib-cf: 2 passed (free-cf): std.random/std.time/io.console over the ColdFire time/rng seam, core.convert"

## ---- the input-dependent corpus (tests/kp-io) ----
## Programs that read stdin or take arguments: a .input file is the program's
## stdin and a .args file its command line (ParamStr/ParamCount), with the
## .expected the reference kprun's output under the same.  This is the one surface
## the other corpora omit (they run with no input), so it exercises the runner's
## stdin/argv plumbing across every tier.  x86 JIT and AOT are in check-all; the
## rv32 and arm64 AOT paths and the freestanding tier run in check-emu-all, so the
## entry argument vector and the read seam are checked on the no-libc tier too.
check-kp-io: build/skj-kp-jit build/skj-kpc build/skj-kp build/libkpaot.a
	@QEMU= KP_SRCDIR=tests/kp-io MODE=jit JIT=build/skj-kp-jit sh tests/run-kp-src-tests.sh
	@KP_SRCDIR=tests/kp-io MODE=aot FRONTEND=build/skj-kpc \
	    KPAOT=build/skj-kp KPAOTLIB=build/libkpaot.a \
	    KPAOTCC=$(CC) KPAOTAS="$(X64_ASM) $(X64_ASMFLAGS)" \
	    sh tests/run-kp-src-tests.sh

check-kp-io-rv: build/skj-kpc-rv build/skj-kp-rv build/libkpaot-rv.a
	@KP_SRCDIR=tests/kp-io MODE=aot FRONTEND=build/skj-kpc-rv \
	    KPAOT=build/skj-kp-rv KPAOTLIB=build/libkpaot-rv.a \
	    KPAOTCC="$(RVJIT_CC)" KPAOTQEMU="$(QEMU_RV)" \
	    sh tests/run-kp-src-tests.sh

check-kp-io-arm64: build/skj-kpc-arm64 build/skj-kp-arm64 build/libkpaot-arm64.a
	@KP_SRCDIR=tests/kp-io MODE=aot FRONTEND=build/skj-kpc-arm64 \
	    KPAOT=build/skj-kp-arm64 KPAOTLIB=build/libkpaot-arm64.a \
	    KPAOTCC="$(A64_CC)" KPAOTQEMU="$(QEMU_A64)" \
	    sh tests/run-kp-src-tests.sh

check-kp-io-free: build/skj-kpc-rv build/skj-kp-rv build/skj-as-rv build/skj-ld-rv \
                  build/kpfree-rv/kp_crt_rv.o build/libkpaot-rv-free.a
	@KP_SRCDIR=tests/kp-io MODE=free FRONTEND=build/skj-kpc-rv \
	    KPRV=build/skj-kp-rv KPAS=build/skj-as-rv KPLD=build/skj-ld-rv \
	    KPCRT=build/kpfree-rv/kp_crt_rv.o KPLIB=build/libkpaot-rv-free.a \
	    QEMU="$(QEMU_RV)" \
	    sh tests/run-kp-src-tests.sh

## ---- the include-file corpus (tests/kp-inc) ----
## Programs that pull in a {$I ...} include file, the last input kind: the
## directive splices the named file's text at the point it appears, resolved at
## compile time against the source directory, so the .inc sits beside the .pas and
## needs no harness plumbing.  incexpr includes expressions (a string and a number
## constant), incdecl a whole procedure declaration.  The .expected is the
## reference kprun's output.  x86 JIT and AOT are in check-all; the rv32 and arm64
## AOT paths and the freestanding tier run in check-emu-all (the include is
## resolved by the host front end in every case, so the included text is in the
## module before codegen).
check-kp-inc: build/skj-kp-jit build/skj-kpc build/skj-kp build/libkpaot.a
	@QEMU= KP_SRCDIR=tests/kp-inc MODE=jit JIT=build/skj-kp-jit sh tests/run-kp-src-tests.sh
	@KP_SRCDIR=tests/kp-inc MODE=aot FRONTEND=build/skj-kpc \
	    KPAOT=build/skj-kp KPAOTLIB=build/libkpaot.a \
	    KPAOTCC=$(CC) KPAOTAS="$(X64_ASM) $(X64_ASMFLAGS)" \
	    sh tests/run-kp-src-tests.sh

check-kp-inc-rv: build/skj-kpc-rv build/skj-kp-rv build/libkpaot-rv.a
	@KP_SRCDIR=tests/kp-inc MODE=aot FRONTEND=build/skj-kpc-rv \
	    KPAOT=build/skj-kp-rv KPAOTLIB=build/libkpaot-rv.a \
	    KPAOTCC="$(RVJIT_CC)" KPAOTQEMU="$(QEMU_RV)" \
	    sh tests/run-kp-src-tests.sh

check-kp-inc-arm64: build/skj-kpc-arm64 build/skj-kp-arm64 build/libkpaot-arm64.a
	@KP_SRCDIR=tests/kp-inc MODE=aot FRONTEND=build/skj-kpc-arm64 \
	    KPAOT=build/skj-kp-arm64 KPAOTLIB=build/libkpaot-arm64.a \
	    KPAOTCC="$(A64_CC)" KPAOTQEMU="$(QEMU_A64)" \
	    sh tests/run-kp-src-tests.sh

check-kp-inc-free: build/skj-kpc-rv build/skj-kp-rv build/skj-as-rv build/skj-ld-rv \
                   build/kpfree-rv/kp_crt_rv.o build/libkpaot-rv-free.a
	@KP_SRCDIR=tests/kp-inc MODE=free FRONTEND=build/skj-kpc-rv \
	    KPRV=build/skj-kp-rv KPAS=build/skj-as-rv KPLD=build/skj-ld-rv \
	    KPCRT=build/kpfree-rv/kp_crt_rv.o KPLIB=build/libkpaot-rv-free.a \
	    QEMU="$(QEMU_RV)" \
	    sh tests/run-kp-src-tests.sh

## ---- the freestanding guest runtime tier (downstream hosts, e.g. boris) ----
## libkpaot-rv-free.a is the re-bottoming of the hosted libkpaot-rv.a for a host
## whose seam to the guest is only write/read/exit, with the guest heap handed in
## at entry.  The compute runtime (str_core/dynarr/map) is the same source the
## hosted archive builds; the platform is rt_free.c (the guest heap over the
## handed-in region, ARC, the traps) and io_free.c (the console and the integer
## Str/Val over the syscalls), and the crt is kp_crt_rv.S (assembled by skj-as-rv).
## The 64-bit integer helpers match the hosted archive: the backend's stack-ABI
## __muldi3 and shift family resolve through kobold/rv_i64_aot_alias.S into
## jit/rv_jit_i64.S, and the register-ABI __divdi3 family comes from soft64.c built
## -DSOFT64_NO_MULSHIFT (so its own stack-ABI-incompatible __muldi3 is dropped and
## does not clash with the alias).  Built with the rv cross cc at -fno-pic
## -mno-relax so skj-ld-rv links it, double reals to match skj-kp-rv, and
## KP_FREESTANDING for the glue.
# Deferred (=, not :=) because RV_RT_CFLAGS is defined further down the Makefile.
# The C runtime is compiled by skj-cc-rv to GAS asm and assembled by skj-as-rv,
# so the tier needs no external toolchain (smolmoo/aldeby self-build it from the
# skj toolchain alone), matching the ColdFire tier.  skj-cc-rv ignores the
# gcc-style flags, so only the defines and the include path carry over; it emits
# the RISC-V ILP32 psABI, which the hand-written .S pieces below also use.
KPFREE_RV_SKJFLAGS := -DKP_FREESTANDING -DKP_REAL=double -Ikobold -Iir

build/kpfree-rv:
	mkdir -p build/kpfree-rv

build/kpfree-rv/%.o: kobold/%.c $(KP_HDRS_RV) build/skj-cc-rv build/skj-as-rv Makefile | build/kpfree-rv
	./build/skj-cc-rv $(KPFREE_RV_SKJFLAGS) -o $(@:.o=.s) $<
	./build/skj-as-rv -o $@ $(@:.o=.s)

# soft64.c supplies only the 64-bit divide family here (-DSOFT64_NO_MULSHIFT);
# the mul/shift helpers are the hand-written .S aliases below, which the rv
# backend's internal stack ABI calls.
build/kpfree-rv/soft64.o: runtime/soft64.c build/skj-cc-rv build/skj-as-rv Makefile | build/kpfree-rv
	./build/skj-cc-rv $(KPFREE_RV_SKJFLAGS) -DSOFT64_NO_MULSHIFT -o $(@:.o=.s) runtime/soft64.c
	./build/skj-as-rv -o $@ $(@:.o=.s)

# The RNG generators and their guest-facing glue (runtime/, not kobold/).
build/kpfree-rv/rng.o: runtime/rng.c runtime/rng.h build/skj-cc-rv build/skj-as-rv Makefile | build/kpfree-rv
	./build/skj-cc-rv $(KPFREE_RV_SKJFLAGS) -o $(@:.o=.s) runtime/rng.c
	./build/skj-as-rv -o $@ $(@:.o=.s)

build/kpfree-rv/rng_glue.o: runtime/rng_glue.c runtime/rng.h build/skj-cc-rv build/skj-as-rv Makefile | build/kpfree-rv
	./build/skj-cc-rv $(KPFREE_RV_SKJFLAGS) -o $(@:.o=.s) runtime/rng_glue.c
	./build/skj-as-rv -o $@ $(@:.o=.s)

# The park / yield glue (Sleep / Yield over the nanosleep / sched_yield seam).
build/kpfree-rv/sched_glue.o: runtime/sched_glue.c build/skj-cc-rv build/skj-as-rv Makefile | build/kpfree-rv
	./build/skj-cc-rv $(KPFREE_RV_SKJFLAGS) -o $(@:.o=.s) runtime/sched_glue.c
	./build/skj-as-rv -o $@ $(@:.o=.s)

build/kpfree-rv/kp_crt_rv.o: kobold/kp_crt_rv.S runtime/crt_entry_rv.S build/skj-as-rv | build/kpfree-rv
	./build/skj-as-rv -o $@ $<

# The i64 helpers (tail-jumps into the skj_jit_* routines) and the coroutine
# context switch (fsd/fld) assemble with skj-as-rv: it has the tail pseudo and
# the double float/store forms, so the freestanding tier needs no external
# toolchain (co_free.c goes through the generic C rule above).
build/kpfree-rv/rv_i64_aot_alias.o: kobold/rv_i64_aot_alias.S build/skj-as-rv | build/kpfree-rv
	./build/skj-as-rv -o $@ $<

build/kpfree-rv/rv_jit_i64.o: jit/rv_jit_i64.S runtime/i64_rv.S build/skj-as-rv | build/kpfree-rv
	./build/skj-as-rv -o $@ $<

build/kpfree-rv/co_switch_rv.o: runtime/co_switch_rv.S build/skj-as-rv | build/kpfree-rv
	./build/skj-as-rv -o $@ $<

# The host-seam syscall wrappers (ecall) live in asm rather than io_free.c, so a
# compiler without extended inline asm (skj-cc-rv) can build the console C.
build/kpfree-rv/io_free_rv.o: runtime/io_free_rv.S runtime/syscall_rv.S build/skj-as-rv | build/kpfree-rv
	./build/skj-as-rv -o $@ $<

# libc's buffered-stream core backs the freestanding console I/O, so the Kobold
# runtime and the C runtime share one stdio buffering layer.  It builds on the
# write/read seam only (no malloc, no printf engine), so it links in alone.
build/kpfree-rv/stdio_core.o: libc/stdio_core.c libc/stdio_impl.h $(wildcard include/*.h) build/skj-cc-rv build/skj-as-rv Makefile | build/kpfree-rv
	./build/skj-cc-rv -Iinclude -Ilibc -o $(@:.o=.s) libc/stdio_core.c
	./build/skj-as-rv -o $@ $(@:.o=.s)

## strtod + ctype: io_free.c's real parse (scan_real) is shared from libc's strtod.
build/kpfree-rv/strtod.o: libc/strtod.c $(wildcard include/*.h) build/skj-cc-rv build/skj-as-rv Makefile | build/kpfree-rv
	./build/skj-cc-rv -Iinclude -Ilibc -o $(@:.o=.s) libc/strtod.c
	./build/skj-as-rv -o $@ $(@:.o=.s)

build/kpfree-rv/ctype.o: libc/ctype.c $(wildcard include/*.h) build/skj-cc-rv build/skj-as-rv Makefile | build/kpfree-rv
	./build/skj-cc-rv -Iinclude -Ilibc -o $(@:.o=.s) libc/ctype.c
	./build/skj-as-rv -o $@ $(@:.o=.s)

## numfmt: the libgcc-free number formatters io_free.c shares with libc's printf.
build/kpfree-rv/numfmt.o: libc/numfmt.c libc/fmt.h $(wildcard include/*.h) build/skj-cc-rv build/skj-as-rv Makefile | build/kpfree-rv
	./build/skj-cc-rv -Iinclude -Ilibc -o $(@:.o=.s) libc/numfmt.c
	./build/skj-as-rv -o $@ $(@:.o=.s)

## io_free.c's file-metadata ops (kpf_exists/size/isdir/delete/rename/mkdir) need
## struct stat and stat/mkdir, so this one object is built with the libc include
## path on top of the freestanding flags; the stat/fileops wrappers below supply
## stat/unlink/rename/mkdir over the crt's statx and *at syscalls.
## The Kobold freestanding file I/O routes through the shared fs/ layer, the
## same seam the C libc uses (SKJ_FS_INTERPOSE), with the temporary ambient host
## backend (FS_WITH_AMBIENT).  io_free.c is the Kobold-side shim; stat.c and
## fileops.c are the shared libc path ops its metadata calls funnel through.
KPFREE_RV_FS := -DSKJ_FS_INTERPOSE -DFS_WITH_AMBIENT -DFS_WITH_HOSTCAP -Ifs
build/kpfree-rv/io_free.o: kobold/io_free.c $(KP_HDRS_RV) $(FS_HDR) $(wildcard include/*.h) $(wildcard include/sys/*.h) build/skj-cc-rv build/skj-as-rv Makefile | build/kpfree-rv
	./build/skj-cc-rv $(KPFREE_RV_SKJFLAGS) -Iinclude -Ilibc $(KPFREE_RV_FS) -o $(@:.o=.s) kobold/io_free.c
	./build/skj-as-rv -o $@ $(@:.o=.s)
build/kpfree-rv/stat.o: libc/stat.c $(FS_HDR) $(wildcard include/*.h) $(wildcard include/sys/*.h) build/skj-cc-rv build/skj-as-rv Makefile | build/kpfree-rv
	./build/skj-cc-rv -Iinclude -Ilibc $(KPFREE_RV_FS) -o $(@:.o=.s) libc/stat.c
	./build/skj-as-rv -o $@ $(@:.o=.s)
build/kpfree-rv/fileops.o: libc/fileops.c $(FS_HDR) $(wildcard include/*.h) $(wildcard include/sys/*.h) build/skj-cc-rv build/skj-as-rv Makefile | build/kpfree-rv
	./build/skj-cc-rv -Iinclude -Ilibc $(KPFREE_RV_FS) -o $(@:.o=.s) libc/fileops.c
	./build/skj-as-rv -o $@ $(@:.o=.s)
## The brk-bump both runtimes share: rt_free.c's gheap grows through __libc_sbrk
## (libc/sbrk.c), the same source libc's malloc uses.  skj-cc-rv defines
## __skj_ilp32__, so sbrk.c takes its brk path, not the LP64 fixed-region one.
build/kpfree-rv/sbrk.o: libc/sbrk.c $(wildcard include/*.h) build/skj-cc-rv build/skj-as-rv Makefile | build/kpfree-rv
	./build/skj-cc-rv -Iinclude -Ilibc -o $(@:.o=.s) libc/sbrk.c
	./build/skj-as-rv -o $@ $(@:.o=.s)

KPFREE_RV_ARCHIVE := build/kpfree-rv/rt_free.o build/kpfree-rv/io_free.o build/kpfree-rv/kp_io.o \
                     build/kpfree-rv/io_free_rv.o build/kpfree-rv/sbrk.o \
                     build/kpfree-rv/fs.o build/kpfree-rv/ramfs.o build/kpfree-rv/ambientfs.o build/kpfree-rv/hostcap.o \
                     build/kpfree-rv/numfmt.o build/kpfree-rv/stat.o build/kpfree-rv/fileops.o \
                     build/kpfree-rv/strtod.o build/kpfree-rv/ctype.o \
                     build/kpfree-rv/str_core.o build/kpfree-rv/dynarr.o \
                     build/kpfree-rv/map.o build/kpfree-rv/soft64.o \
                     build/kpfree-rv/rv_i64_aot_alias.o build/kpfree-rv/rv_jit_i64.o \
                     build/kpfree-rv/co_free.o build/kpfree-rv/co_switch_rv.o \
                     build/kpfree-rv/math_free.o build/kpfree-rv/rng.o \
                     build/kpfree-rv/rng_glue.o build/kpfree-rv/sched_glue.o

build/libkpaot-rv-free.a: $(KPFREE_RV_ARCHIVE) | build/kpfree-rv
	@rm -f $@
	$(AR) rcs $@ $(KPFREE_RV_ARCHIVE)

## ---- the freestanding guest runtime tier for ColdFire (downstream host: boris) ----
## libkpaot-cf-free.a is the ColdFire sibling of libkpaot-rv-free.a: the same
## compute runtime (str_core/dynarr/map) and freestanding platform (rt_free.c, the
## guest heap over a handed-in region) over a ColdFire console/exit seam, for a host
## whose seam to the guest is only write/read/exit.  Two things differ from the rv
## tier.  First, the 64-bit integer helpers are soft64.c alone, built WITHOUT
## -DSOFT64_NO_MULSHIFT: cf_emit emits jsr __muldi3 and the shift family under the
## toolkit stack convention, which is the m68k C ABI, so soft64.c's C helpers are
## ABI-compatible as emitted and need no stack-ABI alias (the rv tier's alias .S is
## a soft-float ilp32 quirk that does not arise here).  Second, the crt and the
## syscall seam are ColdFire: TRAP #0 with the number in d0 (write 4, read 3,
## exit 1).  Built strict-ColdFire (-mcpu=5475) with M68K_CC so skj-run's CFV4e
## core runs it, and assembled by skj-as to keep the path binutils-free.
# Deferred (=, not :=): CF_RT_CFLAGS is defined further down the Makefile.
KP_HDRS_CF := kobold/str.h kobold/dynarr.h kobold/map.h kobold/obj.h kobold/gheap_core.h kobold/co.h \
              kobold/handle.h kobold/lowmem.h kobold/rt_glue.h kobold/kp_abi.h ir/target.h
# The C runtime is compiled by skj-cc (the in-tree C compiler) to GAS asm and
# assembled by skj-as, so the ColdFire kp runtime is built with no external
# toolchain (the host-seam syscalls and the fp0 math shim are already skj-as .S).
# skj-cc ignores the gcc-style flags, so only the defines and the include path
# carry over; strict-ColdFire is implicit (the backend targets ColdFire/m68k).
KPFREE_CF_SKJFLAGS := -DKP_FREESTANDING -DKP_REAL=double -Ikobold -Iir

build/kpfree-cf:
	mkdir -p build/kpfree-cf

build/kpfree-cf/%.o: kobold/%.c $(KP_HDRS_CF) build/skj-cc build/skj-as Makefile | build/kpfree-cf
	./build/skj-cc $(KPFREE_CF_SKJFLAGS) -o $(@:.o=.s) $<
	./build/skj-as -o $@ $(@:.o=.s)

# soft64.c carries the full 64-bit helper set for ColdFire (mul/shift/div); skj-cc
# emits the toolkit stack convention, which is the m68k C ABI the backend's
# __muldi3 helper calls use, so no -DSOFT64_NO_MULSHIFT and no alias is needed.
build/kpfree-cf/soft64.o: runtime/soft64.c build/skj-cc build/skj-as Makefile | build/kpfree-cf
	./build/skj-cc $(KPFREE_CF_SKJFLAGS) -o $(@:.o=.s) runtime/soft64.c
	./build/skj-as -o $@ $(@:.o=.s)

# The crt is assembled by skj-as (binutils-free), like the rv tier's skj-as-rv.
# It is the start file, linked alongside the archive, not a member of it.
build/kpfree-cf/kp_crt_cf.o: kobold/kp_crt_cf.S runtime/crt_entry_cf.S build/skj-as | build/kpfree-cf
	./build/skj-as -o $@ $<

# The coroutine context switch has an existing skj-as rule (build/co_switch_cf.o),
# reused here.  math_free supplies the transcendentals: its double-math core (my_*)
# is shared with the rv tier, and its ColdFire h_* entries are plain C wrappers
# (skj-cc returns a double in fp0, the convention the kp call reads), so no asm
# shim is needed (the kpcom seed imports none, so this only matters to the
# transcendental corpus).

# The host-seam syscall wrappers (trap #0) live in asm rather than io_free.c, so
# a compiler without extended inline asm (skj-cc) can build the console C.
build/kpfree-cf/io_free_cf.o: runtime/io_free_cf.S runtime/syscall_cf.S build/skj-as | build/kpfree-cf
	./build/skj-as -o $@ $<

# libc's buffered-stream core backs the freestanding console I/O (see the rv rule
# above); it builds on the write/read seam only.
build/kpfree-cf/stdio_core.o: libc/stdio_core.c libc/stdio_impl.h $(wildcard include/*.h) build/skj-cc build/skj-as Makefile | build/kpfree-cf
	./build/skj-cc -Iinclude -Ilibc -o $(@:.o=.s) libc/stdio_core.c
	./build/skj-as -o $@ $(@:.o=.s)

# strtod + ctype: io_free.c's real parse (scan_real) is shared from libc's strtod.
build/kpfree-cf/strtod.o: libc/strtod.c $(wildcard include/*.h) build/skj-cc build/skj-as Makefile | build/kpfree-cf
	./build/skj-cc -Iinclude -Ilibc -o $(@:.o=.s) libc/strtod.c
	./build/skj-as -o $@ $(@:.o=.s)

build/kpfree-cf/ctype.o: libc/ctype.c $(wildcard include/*.h) build/skj-cc build/skj-as Makefile | build/kpfree-cf
	./build/skj-cc -Iinclude -Ilibc -o $(@:.o=.s) libc/ctype.c
	./build/skj-as -o $@ $(@:.o=.s)

## numfmt: the libgcc-free number formatters io_free.c shares with libc's printf.
build/kpfree-cf/numfmt.o: libc/numfmt.c libc/fmt.h $(wildcard include/*.h) build/skj-cc build/skj-as Makefile | build/kpfree-cf
	./build/skj-cc -Iinclude -Ilibc -o $(@:.o=.s) libc/numfmt.c
	./build/skj-as -o $@ $(@:.o=.s)

## io_free.c's file-metadata ops need struct stat and stat/mkdir (see the rv rule).
## Kobold freestanding file I/O through the shared fs/ layer (the cf sibling of
## the rv rules): the Kobold-side io_free.c shim plus the shared stat.c/fileops.c
## path ops, built with SKJ_FS_INTERPOSE and the temporary ambient host backend.
KPFREE_CF_FS := -DSKJ_FS_INTERPOSE -DFS_WITH_AMBIENT -DFS_WITH_HOSTCAP -Ifs
FS_FREE_CC_CF := ./build/skj-cc -Iinclude -Ilibc -Ifs -DFS_WITH_AMBIENT -DFS_WITH_HOSTCAP -DFS_AMBIENT_DEFAULT
build/kpfree-cf/io_free.o: kobold/io_free.c $(KP_HDRS_CF) $(FS_HDR) $(wildcard include/*.h) $(wildcard include/sys/*.h) build/skj-cc build/skj-as Makefile | build/kpfree-cf
	./build/skj-cc $(KPFREE_CF_SKJFLAGS) -Iinclude -Ilibc $(KPFREE_CF_FS) -o $(@:.o=.s) kobold/io_free.c
	./build/skj-as -o $@ $(@:.o=.s)
build/kpfree-cf/stat.o: libc/stat.c $(FS_HDR) $(wildcard include/*.h) $(wildcard include/sys/*.h) build/skj-cc build/skj-as Makefile | build/kpfree-cf
	./build/skj-cc -Iinclude -Ilibc $(KPFREE_CF_FS) -o $(@:.o=.s) libc/stat.c
	./build/skj-as -o $@ $(@:.o=.s)
build/kpfree-cf/fileops.o: libc/fileops.c $(FS_HDR) $(wildcard include/*.h) $(wildcard include/sys/*.h) build/skj-cc build/skj-as Makefile | build/kpfree-cf
	./build/skj-cc -Iinclude -Ilibc $(KPFREE_CF_FS) -o $(@:.o=.s) libc/fileops.c
	./build/skj-as -o $@ $(@:.o=.s)
build/kpfree-cf/fs.o: fs/fs.c $(FS_HDR) $(wildcard include/*.h) build/skj-cc build/skj-as Makefile | build/kpfree-cf
	$(FS_FREE_CC_CF) -o $(@:.o=.s) fs/fs.c
	./build/skj-as -o $@ $(@:.o=.s)
build/kpfree-cf/ramfs.o: fs/ramfs.c $(FS_HDR) $(wildcard include/*.h) build/skj-cc build/skj-as Makefile | build/kpfree-cf
	$(FS_FREE_CC_CF) -o $(@:.o=.s) fs/ramfs.c
	./build/skj-as -o $@ $(@:.o=.s)
build/kpfree-cf/ambientfs.o: fs/ambientfs.c $(FS_HDR) $(wildcard include/*.h) build/skj-cc build/skj-as Makefile | build/kpfree-cf
	$(FS_FREE_CC_CF) -o $(@:.o=.s) fs/ambientfs.c
	./build/skj-as -o $@ $(@:.o=.s)
build/kpfree-cf/hostcap.o: fs/hostcap.c $(FS_HDR) $(wildcard include/*.h) build/skj-cc build/skj-as Makefile | build/kpfree-cf
	$(FS_FREE_CC_CF) -o $(@:.o=.s) fs/hostcap.c
	./build/skj-as -o $@ $(@:.o=.s)
## The shared brk-bump (see the rv rule above): rt_free.c's gheap over __libc_sbrk.
build/kpfree-cf/sbrk.o: libc/sbrk.c $(wildcard include/*.h) build/skj-cc build/skj-as Makefile | build/kpfree-cf
	./build/skj-cc -Iinclude -Ilibc -o $(@:.o=.s) libc/sbrk.c
	./build/skj-as -o $@ $(@:.o=.s)

# The RNG generators and their guest-facing glue (runtime/, not kobold/), and the
# park / yield glue, over the ColdFire time/rng/yield syscall wrappers now in
# syscall_cf.S.  The rv tier's siblings are above.
build/kpfree-cf/rng.o: runtime/rng.c runtime/rng.h build/skj-cc build/skj-as Makefile | build/kpfree-cf
	./build/skj-cc $(KPFREE_CF_SKJFLAGS) -o $(@:.o=.s) runtime/rng.c
	./build/skj-as -o $@ $(@:.o=.s)

build/kpfree-cf/rng_glue.o: runtime/rng_glue.c runtime/rng.h build/skj-cc build/skj-as Makefile | build/kpfree-cf
	./build/skj-cc $(KPFREE_CF_SKJFLAGS) -o $(@:.o=.s) runtime/rng_glue.c
	./build/skj-as -o $@ $(@:.o=.s)

build/kpfree-cf/sched_glue.o: runtime/sched_glue.c build/skj-cc build/skj-as Makefile | build/kpfree-cf
	./build/skj-cc $(KPFREE_CF_SKJFLAGS) -o $(@:.o=.s) runtime/sched_glue.c
	./build/skj-as -o $@ $(@:.o=.s)

KPFREE_CF_ARCHIVE := build/kpfree-cf/rt_free.o build/kpfree-cf/io_free.o build/kpfree-cf/kp_io.o \
                     build/kpfree-cf/io_free_cf.o build/kpfree-cf/sbrk.o \
                     build/kpfree-cf/fs.o build/kpfree-cf/ramfs.o build/kpfree-cf/ambientfs.o build/kpfree-cf/hostcap.o \
                     build/kpfree-cf/numfmt.o build/kpfree-cf/stat.o build/kpfree-cf/fileops.o \
                     build/kpfree-cf/strtod.o build/kpfree-cf/ctype.o \
                     build/kpfree-cf/str_core.o build/kpfree-cf/dynarr.o \
                     build/kpfree-cf/map.o build/kpfree-cf/soft64.o \
                     build/kpfree-cf/co_free.o build/kpfree-cf/math_free.o \
                     build/kpfree-cf/rng.o build/kpfree-cf/rng_glue.o \
                     build/kpfree-cf/sched_glue.o \
                     build/co_switch_cf.o

build/libkpaot-cf-free.a: $(KPFREE_CF_ARCHIVE) | build/kpfree-cf
	@rm -f $@
	$(AR) rcs $@ $(KPFREE_CF_ARCHIVE)

## check-kp-free: the boris definition-of-done pipeline, from Kobold source to a
## freestanding rv32 ELF, with nothing but the skj toolchain past the front end.
## The host skj-kpc emits the module, skj-kp-rv compiles it, skj-as-rv assembles,
## and skj-ld-rv links the crt, the program, and libkpaot-rv-free.a into a static
## ELF whose only host needs are write/read/exit and the handed-in heap.  Run under
## qemu-riscv32 (which trivially provides that seam); stdout must match .expected.
check-kp-free: build/skj-kpc-rv build/skj-kp-rv build/skj-as-rv build/skj-ld-rv \
               build/kpfree-rv/kp_crt_rv.o build/libkpaot-rv-free.a
	@MODE=free FRONTEND=build/skj-kpc-rv \
	    KPRV=build/skj-kp-rv KPAS=build/skj-as-rv KPLD=build/skj-ld-rv \
	    KPCRT=build/kpfree-rv/kp_crt_rv.o KPLIB=build/libkpaot-rv-free.a \
	    QEMU="$(QEMU_RV)" \
	    sh tests/run-kp-src-tests.sh

## The ColdFire freestanding kp AOT path, run on the in-tree emulator (qemu does
## not model CFV4e).  skj-kp-cf emits big-endian ColdFire asm from the one
## agnostic seed, skj-as/skj-ld (over kp_cf.ld, a 32M-RAM script the crt needs)
## build the ELF, and skj-run executes it.  This is what proves a boris host can
## compile Kobold to native ColdFire from the seed alone, and the endian-neutral
## constant emission (strobj, typed .init word arrays) running big-endian.  The
## full kp-src set passes: coroutines (the ColdFire co_free seed and the argument
## liveness fix) and the transcendentals (math_free's ColdFire h_* shims) included.
check-kp-free-cf: build/skj-kpc-cf build/skj-kp-cf build/skj-as build/skj-ld \
                  build/skj-run build/kpfree-cf/kp_crt_cf.o build/libkpaot-cf-free.a
	@MODE=free FRONTEND=build/skj-kpc-cf \
	    KPRV=build/skj-kp-cf KPAS=build/skj-as KPLD="build/skj-ld -T kobold/kp_cf.ld" \
	    KPCRT=build/kpfree-cf/kp_crt_cf.o KPLIB=build/libkpaot-cf-free.a \
	    QEMU=build/skj-run \
	    sh tests/run-kp-src-tests.sh

## skj-jit-arm64: the in-process AArch64 JIT.  Same driver and shared core as
## skj-jit, over the AArch64 byte sink (jit/jit_arm64.c) and encoder
## (jit/emit_arm64.c) driving the shared selector (backend/arm64_select.c).
## Cross-built with the arm64 toolchain and run in-process under qemu-aarch64
## (qemu-user executes the JIT-generated code).
JIT_SRC_A64 := jit/main.c jit/jit_common.c jit/jit_arena.c jit/jit_arm64.c jit/emit_arm64.c \
               jit/code.c jit/call_guest_arm64.S \
               backend/arm64_select.c backend/regalloc_arm64.c \
               $(JIT_FE) $(CC_CPP) $(IR_SRC)
build/skj-jit-arm64: $(JIT_SRC_A64) cc/cc.h jit/jit_common.h jit/jit_arena.h jit/jit_x86.h jit/emit_arm64.h jit/code.h backend/arm64_mc.h backend/arm64_select.h ir/ir.h Makefile build/version.h | build
	$(A64_CC) $(CFLAGS) -DSKJ_LP64=1 -DCC_PSABI -DCC_ARM64 $(SKJ_INC_DEF) -Icc -Icpp -Iir -Ijit -Ibackend -o $@ $(JIT_SRC_A64)

## Run the C suite in-process through skj-jit-arm64 under qemu-aarch64.  The
## arm64 JIT supports _Float16 natively (fcvt), so it excludes only inline asm.
check-jit-arm64: build/skj-jit-arm64
	@JIT="$(CURDIR)/build/skj-jit-arm64" RUNNER="$(QEMU_A64)" \
	 EXCLUDE="cc_t077_inline_asm" sh tests/run-jit-tests.sh

## test-jit-arm64-high: prove the arm64 C JIT is high-address-safe.  Run the
## self-checking varargs+pointer test through skj-jit-arm64 (under qemu-aarch64)
## with the guest regions forced above 4GB (SKJ_JIT_MAP_HIGH), and require the
## same exit code as a normal run.  This exercises the uniform 64-bit integer
## home (ldrx address reloads) at a base with nonzero high bits; a 32-bit reload
## would truncate the base and fault or miscompute.
test-jit-arm64-high: build/skj-jit-arm64
	@t=tests/cc_t081_varargs_stdarg.c; want=`cat tests/cc_t081_varargs_stdarg.exitcode`; \
	 set +e; \
	 SKJ_JIT_MAP_HIGH=1 $(QEMU_A64) ./build/skj-jit-arm64 $$t >build/jit-arm64-high.out 2>&1; \
	 rc=$$?; set -e; \
	 if [ "$$rc" -ne "$$want" ]; then \
	     echo "FAIL: skj-jit-arm64 (guest >4GB) exit $$rc, want $$want"; \
	     sed 's/^/    /' build/jit-arm64-high.out; exit 1; fi; \
	 echo "PASS: skj-jit-arm64 varargs+pointers correct with guest mapped >4GB (exit $$rc)"

## Validate the arm64 byte sink: disassemble each JIT-compiled test's code and
## confirm every 4-byte word is a legal instruction (composed-encoding coverage
## the per-instruction oracle and the exit-code suite do not give on their own).
check-jit-arm64-disasm: build/skj-jit-arm64
	@JIT="$(CURDIR)/build/skj-jit-arm64" RUNNER="$(QEMU_A64)" \
	 OBJDUMP="aarch64-linux-gnu-objdump" sh tests/run-jit-disasm.sh

## The JIT div-by-zero guard (TRAP_GUARDED): a runtime divide by zero must raise
## the fault and exit 70, on both JIT targets.  The C suite never divides by a
## runtime zero, so this is the only exercise of that path.
test-jit-trap: build/skj-jit
	@rc=0; ./build/skj-jit tests/jit_divzero.c >/dev/null 2>&1 || rc=$$?; \
	 if [ "$$rc" -eq 70 ]; then echo "PASS  jit-trap (x86, exit 70)"; \
	 else echo "FAIL  jit-trap (x86, exit $$rc, want 70)"; exit 1; fi

test-jit-trap-arm64: build/skj-jit-arm64
	@rc=0; $(QEMU_A64) ./build/skj-jit-arm64 tests/jit_divzero.c >/dev/null 2>&1 || rc=$$?; \
	 if [ "$$rc" -eq 70 ]; then echo "PASS  jit-trap (arm64, exit 70)"; \
	 else echo "FAIL  jit-trap (arm64, exit $$rc, want 70)"; exit 1; fi

## skj-jit-rv: the in-process RV32 JIT.  Same driver and shared core as skj-jit,
## over the RISC-V byte sink (jit/jit_rv.c) and encoder (jit/emit_rv.c) driving
## the shared selector (backend/rv_select.c).  RV32 is a register-pair target, so
## the backend calls the stack-ABI 64-bit helpers (jit/rv_jit_i64.S), bound to
## their __*di3 names in the driver.  Cross-built for hosted rv32 with zig cc and
## run in-process under qemu-riscv32.  Unlike arm64/x86, RV32 division does not
## fault on a zero divisor and the backend emits no guard (matching skj-cc-rv), so
## there is no div-by-zero trap test here.
JIT_SRC_RV := jit/main.c jit/jit_common.c jit/jit_arena.c jit/jit_rv.c jit/emit_rv.c \
              jit/code.c jit/call_guest_rv.S jit/rv_jit_i64.S \
              backend/rv_select.c backend/regalloc_rv.c \
              $(JIT_FE) $(CC_CPP) $(IR_SRC)
build/skj-jit-rv: $(JIT_SRC_RV) cc/cc.h jit/jit_common.h jit/jit_arena.h jit/jit_x86.h jit/emit_rv.h jit/code.h backend/rv_mc.h backend/rv_select.h ir/ir.h runtime/i64_rv.S Makefile build/version.h | build
	$(RVJIT_CC) $(CFLAGS) -DCC_RISCV $(SKJ_INC_DEF) -Icc -Icpp -Iir -Ijit -Ibackend -o $@ $(JIT_SRC_RV)

## Run the C suite in-process through skj-jit-rv under qemu-riscv32.  The RV JIT
## supports _Float16 natively (Zfh flh/fcvt), so beyond inline asm it excludes
## only what the RV psABI backend itself does not implement (the same cc_t071
## varargs test check-cc-rv excludes via RV_EXCLUDE).
check-jit-rv: build/skj-jit-rv
	@JIT="$(CURDIR)/build/skj-jit-rv" RUNNER="$(QEMU_RV)" \
	 EXCLUDE="$(RV_EXCLUDE) cc_t077_inline_asm" sh tests/run-jit-tests.sh

## Disassembly-legality sweep of the RV32 byte sink, the counterpart of
## check-jit-arm64-disasm: JIT each cc test, dump its code region, and confirm
## riscv objdump decodes every instruction to a legal one.  This catches a
## composed-encoding bug (a wrong li/la sequence, a memop base fallback, or a
## branch patch) that runtime execution would not reach.  The RV sink emits
## only 32-bit encodings, so the linear decode stays aligned.
check-jit-rv-disasm: build/skj-jit-rv
	@JIT="$(CURDIR)/build/skj-jit-rv" RUNNER="$(QEMU_RV)" \
	 OBJDUMP="riscv64-linux-gnu-objdump" MARCH="riscv:rv32" \
	 BIN="$(CURDIR)/build/jitdis-rv.bin" \
	 EXCLUDE="$(RV_EXCLUDE) cc_t077_inline_asm" sh tests/run-jit-disasm.sh


build/skj-cc-mips: $(CC_SRC_MIPS) cc/cc.h cpp/cpp.h cpp/internal.h ir/ir.h | build
	$(CC) $(CFLAGS) -DCC_MIPS $(SKJ_INC_DEF) -Icc -Icpp -Iir -o $@ $(CC_SRC_MIPS)

## The PlayStation (R3051, no coprocessor 1) soft-float C compiler: the same
## sources with -DMIPS_SOFTFLOAT, which emits no cop-1 instruction and lowers
## every float op to a runtime/softfloat.c call.
build/skj-cc-mips-sf: $(CC_SRC_MIPS) cc/cc.h cpp/cpp.h cpp/internal.h ir/ir.h | build
	$(CC) $(CFLAGS) -DCC_MIPS -DMIPS_SOFTFLOAT $(SKJ_INC_DEF) -Icc -Icpp -Iir -o $@ $(CC_SRC_MIPS)

## skj-run emulator driver
#
# The two CPU cores are independent components: a project that vendors
# only one builds with EMU_COLDFIRE=0 or EMU_RV32=0 and the other core's
# sources drop out of the link.
EMU_COLDFIRE ?= 1
EMU_RV32     ?= 1

EMU_SRC   := emu/main.c emu/guest.c emu/hostplat.c emu/elf32.c
EMU_HDR   := emu/guest.h emu/hostplat.h emu/elf32.h fs/fs.h
EMU_FLAGS := -DEMU_COLDFIRE=$(EMU_COLDFIRE) -DEMU_RV32=$(EMU_RV32) -Ifs
ifeq ($(EMU_COLDFIRE),1)
EMU_SRC += emu/coldfire.c emu/cf_user.c
EMU_HDR += emu/coldfire.h
endif
ifeq ($(EMU_RV32),1)
EMU_SRC += emu/rv32.c emu/rv_user.c
EMU_HDR += emu/rv32.h
## The GDB remote-protocol stub (debugger item 6, --gdb PORT), gated by
## SKJ_RV_GDB; a bare emu-rv vendoring omits it.
EMU_SRC += emu/rv_gdb.c
EMU_HDR += emu/rv_gdb.h
EMU_FLAGS += -DSKJ_RV_GDB
endif
## The shared DWARF reader links whenever either core is present, so a guest
## fault prints a symbolized backtrace (debugger item 5).  SKJ_DWARF_BACKTRACE
## gates the code in rv_user.c and cf_user.c; a bare vendoring without dwarf/
## leaves it undefined and still builds, reporting the fault pc alone.
ifneq (,$(filter 1,$(EMU_COLDFIRE) $(EMU_RV32)))
EMU_SRC += dwarf/dwread.c ir/arena.c
EMU_FLAGS += -DSKJ_DWARF_BACKTRACE -Idwarf -Iir
endif

build/skj-run: $(EMU_SRC) $(EMU_HDR) dwarf/dwread.h build/version.h | build
	$(CC) $(CFLAGS) $(EMU_FLAGS) -Iemu -o $@ $(EMU_SRC) -lm

## macOS (and other non-Linux hosts): skj-run is portable C.  The host-varying
## facilities (confined opens, directory enumeration, clock, sleep, random) sit
## behind emu/hostplat.c, the one file that branches on the host OS, so the
## emulator builds with the stock host compiler (clang on macOS) and needs no
## cross toolchain or qemu.  `make macos` builds just the emulator, the tool the
## non-Linux port targets; the rest of the host toolchain (the compilers,
## assembler, linker) is host-native too and builds the same way with `make all`.
## The one path this exercises that is still UNTESTED on a real Darwin machine is
## the cap_walk confined open (see TODO.md); everything else is shared POSIX.
.PHONY: macos
macos: build/skj-run
	@echo "built build/skj-run for $$(uname -s)"

## CI coverage for the portable confinement path: the same emulator with
## -DHP_FORCE_POSIX, which selects the cap_walk component walk in place of the
## Linux openat2 accelerator.  On Linux openat2 would otherwise hide cap_walk, so
## this is how CI compiles and exercises the path every non-Linux host relies on.
build/skj-run-posix: $(EMU_SRC) $(EMU_HDR) dwarf/dwread.h build/version.h | build
	$(CC) $(CFLAGS) $(EMU_FLAGS) -DHP_FORCE_POSIX -Iemu -o $@ $(EMU_SRC) -lm

## ColdFire assembler
AS_SRC := as/main.c as/asm_lex.c as/asm_obj.c as/parse.c as/encode.c as/elf.c \
          dwarf/dwarf.c ir/util.c ir/arena.c

build/skj-as: $(AS_SRC) as/as.h as/asm_lex.h as/asm_obj.h dwarf/dwarf.h | build
	$(CC) $(CFLAGS) -Ias -Iir -Idwarf -o $@ $(AS_SRC)

## RISC-V RV32 assembler (shares the lexer + object helpers with skj-as)
AS_RV_SRC := as/rv_main.c as/asm_lex.c as/asm_obj.c as/rv_parse.c \
             as/rv_encode.c as/rv_macro.c as/rv_elf.c dwarf/dwarf.c \
             ir/util.c ir/arena.c

build/skj-as-rv: $(AS_RV_SRC) as/rv.h as/asm_lex.h as/asm_obj.h dwarf/dwarf.h | build
	$(CC) $(CFLAGS) -Ias -Iir -Idwarf -o $@ $(AS_RV_SRC)

## MIPS I assembler (shares the lexer + object helpers with skj-as)
## Built up in steps; step 2 encodes the instruction set.  See doc/tools/mips.md.
AS_MIPS_SRC := as/mips_main.c as/asm_lex.c as/asm_obj.c as/mips_parse.c \
               as/mips_encode.c as/mips_elf.c as/mips_sched.c dwarf/dwarf.c \
               ir/util.c ir/arena.c

build/skj-as-mips: $(AS_MIPS_SRC) as/mips.h as/asm_lex.h as/asm_obj.h dwarf/dwarf.h | build
	$(CC) $(CFLAGS) -Ias -Iir -Idwarf -o $@ $(AS_MIPS_SRC)

## x86-64 assembler (shares the lexer + object helpers with skj-as).  Reads the
## GNU .intel_syntax noprefix the x86-64 backends emit; no DWARF (the bounded
## input has no .loc).
AS_X64_SRC := as/x86_64_main.c as/asm_lex.c as/asm_obj.c as/x86_64_parse.c \
              as/x86_64_encode.c as/x86_64_elf.c \
              ir/util.c ir/arena.c

build/skj-as-x86-64: $(AS_X64_SRC) as/x86_64.h as/asm_lex.h as/asm_obj.h | build
	$(CC) $(CFLAGS) -Ias -Iir -o $@ $(AS_X64_SRC)

## AArch64 assembler (shares the lexer + object helpers with skj-as).  Reads the
## native GNU as syntax the arm64 backends emit; no DWARF (bounded input).
AS_A64_SRC := as/aarch64_main.c as/asm_lex.c as/asm_obj.c as/aarch64_parse.c \
              as/aarch64_encode.c as/aarch64_elf.c \
              ir/util.c ir/arena.c

build/skj-as-aarch64: $(AS_A64_SRC) as/aarch64.h as/asm_lex.h as/asm_obj.h | build
	$(CC) $(CFLAGS) -Ias -Iir -o $@ $(AS_A64_SRC)

## skj-ld linker
LD_SRC := ld/main.c ld/elf_read.c ld/script.c ld/link.c ld/elf_write.c ld/mapfile.c ld/archive.c ir/util.c ir/arena.c

build/skj-ld: $(LD_SRC) ld/ld.h ld/mapfile.h | build
	$(CC) $(CFLAGS) -Ild -Iir -o $@ $(LD_SRC)

## RISC-V RV32 linker (shares layout core, script parser and mapfile reader)
LD_RV_SRC := ld/rv_main.c ld/rv_elf_read.c ld/rv_link.c ld/rv_elf_write.c \
             ld/rv_archive.c ld/link.c ld/script.c ld/mapfile.c \
             ir/util.c ir/arena.c

build/skj-ld-rv: $(LD_RV_SRC) ld/ld.h ld/rv_ld.h ld/mapfile.h | build
	$(CC) $(CFLAGS) -Ild -Iir -o $@ $(LD_RV_SRC)

## MIPS I linker (shares layout core, script parser and mapfile reader).
## REL relocations with the o32 HI16/LO16 addend carry-pairing.  See doc/tools/mips.md.
LD_MIPS_SRC := ld/mips_main.c ld/mips_elf_read.c ld/mips_link.c \
               ld/mips_elf_write.c ld/mips_psexe_write.c ld/mips_archive.c \
               ld/link.c ld/script.c ld/mapfile.c ir/util.c ir/arena.c

build/skj-ld-mips: $(LD_MIPS_SRC) ld/ld.h ld/mips_ld.h ld/mapfile.h | build
	$(CC) $(CFLAGS) -Ild -Iir -o $@ $(LD_MIPS_SRC)

## skj-ar: a minimal ar(1) archive tool (create with symbol index, list,
## extract).  Generic over the object machine and byte order, so one tool
## serves every target's linker.
AR_SRC := ar/main.c ld/mapfile.c ir/util.c ir/arena.c

build/skj-ar: $(AR_SRC) ld/mapfile.h ir/util.h ir/arena.h | build
	$(CC) $(CFLAGS) -Ild -Iir -o $@ $(AR_SRC)

# Per-test rules: tests/<name>.tc -> build/<name>.s -> build/<name>.o
#                 + build/start.o -> build/<name>
build/%.s: tests/%.tc build/skj-tinc | build
	./build/skj-tinc -o $@ $<

build/%.o: build/%.s
	$(M68K_AS) -o $@ $<

build/start.o: runtime/start.S runtime/crt_entry_cf.S runtime/syscall_cf.S | build
	$(M68K_AS) -Iruntime -o $@ $<

## _Float16 conversion helpers (integer-only, cross-compiled C)
build/half.o: runtime/half.c | build
	$(M68K_CC) -std=c99 -O2 -Wall -ffreestanding -c -o $@ $<

## varargs __va_arg helper (ColdFire toolkit convention)
build/va_cf.o: runtime/va_cf.c | build
	$(M68K_CC) -std=c99 -O2 -Wall -ffreestanding -c -o $@ $<

build/%: build/%.o build/start.o build/skj-ld
	./build/skj-ld -o $@ build/start.o $<

build:
	mkdir -p build

build/version.h: FORCE | build
	@printf '#ifndef SKJ_VERSION_H\n#define SKJ_VERSION_H\n#define SKJ_VERSION "%s"\n#endif\n' '$(VERSION)' > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp

# Every tool's main.c includes "version.h".  This covers the tools outside
# $(TOOLS) too, since they are built by their own targets, not by `make all`.
# The skj-pc LP64 host variants are such tools: they are not in $(TOOLS) but
# build/skj-kpc-rv, -cf, and -arm64 are built from build/skj-pc-x86-64, so a
# bootstrap that starts from a kpcom seed reaches them without `make all` first.
VERSIONED := $(TOOLS) build/skj-tinc-x86-64 build/skj-sc-x86-64 \
             build/skj-pc-x86-64 build/skj-pc-arm64
$(VERSIONED): build/version.h

## Verify version.mk and the git tag agree before cutting a release.
release-check:
	@v=$(SKJ_VERSION); \
	if ! git rev-parse --verify --quiet "refs/tags/v$$v" >/dev/null; then \
	    echo "release-check: no tag v$$v (version.mk says $$v)" >&2; exit 1; \
	fi; \
	if [ "$$(git rev-parse "v$$v^{commit}")" != "$$(git rev-parse HEAD)" ]; then \
	    echo "release-check: tag v$$v is not at HEAD" >&2; exit 1; \
	fi; \
	if [ -n "$$(git status --porcelain)" ]; then \
	    echo "release-check: working tree is dirty" >&2; exit 1; \
	fi; \
	echo "release-check: v$$v ok"

FORCE:

check: build/skj-tinc build/skj-mooc build/skj-pc $(TESTS:tests/%.tc=build/%) $(SCM_TESTS:tests/%.scm=build/%) $(MOO_TESTS:tests/%.moo=build/%) $(MOO_TOY_TESTS:tests/%.moo=build/%) $(PAS_TESTS_NOFLOAT:tests/%.pas=build/%)
	@sh tests/run-tests.sh "$(QEMU)"

## RISC-V per-test rules: tests/<name>.tc -> build/rv/<name>.s -> .o -> binary
RV_TESTS     := $(TESTS)
RV_SCM_TESTS := $(SCM_TESTS)

build/rv/%.s: tests/%.tc build/skj-tinc-rv | build/rv
	./build/skj-tinc-rv -o $@ $<

build/rv/%.o: build/rv/%.s
	$(RV_AS) -march=rv32im -mabi=ilp32 -o $@ $<

build/rv/start.o: runtime/start_rv.S runtime/crt_entry_rv.S runtime/syscall_rv.S runtime/i64_rv.S | build/rv
	$(RV_AS) -Iruntime -march=rv32im -mabi=ilp32 -o $@ $<

build/rv/%: build/rv/%.o build/rv/start.o
	$(RV_LD) -m elf32lriscv -o $@ build/rv/start.o $<

build/rv:
	mkdir -p build/rv

check-rv: build/skj-tinc-rv build/skj-sc-rv $(RV_TESTS:tests/%.tc=build/rv/%) $(RV_SCM_TESTS:tests/%.scm=build/rv/%)
	@sh tests/run-tests.sh "$(QEMU_RV)" build/rv

## Run an ELF under qemu-riscv32 configured to match the rv32.c emulator's ISA.
## Usage: make run-rv ELF=build/rv/foo [ARGS='...']
run-rv:
	@test -n "$(ELF)" || { echo "usage: make run-rv ELF=<path> [ARGS='...']" >&2; exit 2; }
	$(QEMU_RV) -cpu $(QEMU_RV_CPU) $(ELF) $(ARGS)

## MIPS I per-test rules: tests/<name>.tc -> build/mips/<name>.s -> .o -> binary.
## The toolchain (mipsel-none-elf) defaults to -march=mips1, o32,
## little-endian; the generated asm leans on the assembler's default
## `.set reorder` to fill the load/branch delay slots for R2000/R3000.
MIPS_TESTS     := $(TESTS)
MIPS_SCM_TESTS := $(SCM_TESTS)

build/mips/%.s: tests/%.tc build/skj-tinc-mips | build/mips
	./build/skj-tinc-mips -o $@ $<

build/mips/scm_%.s: tests/scm_%.scm build/skj-sc-mips | build/mips
	./build/skj-sc-mips -o $@ $<

build/mips/%.o: build/mips/%.s
	$(MIPS_AS) -march=mips1 -EL -o $@ $<

build/mips/start.o: runtime/start_mips.S | build/mips
	$(MIPS_AS) -march=mips1 -EL -o $@ $<

## The PlayStation BIOS-TTY crt (PS-EXE builds link this instead of start.o).
build/mips/start_psx.o: runtime/start_psx.S | build/mips
	$(MIPS_AS) -march=mips1 -EL -o $@ $<

build/mips/%: build/mips/%.o build/mips/start.o
	$(MIPS_LD) -o $@ build/mips/start.o $<

build/mips:
	mkdir -p build/mips

check-mips: build/skj-tinc-mips build/skj-sc-mips $(MIPS_TESTS:tests/%.tc=build/mips/%) $(MIPS_SCM_TESTS:tests/%.scm=build/mips/%)
	@sh tests/run-tests.sh "$(QEMU_MIPS)" build/mips

## MIPS I integration tests (IR builder -> MIPS asm -> qemu-mipsel).
## Same pattern as the RISC-V variants: a host binary linked with BE_MIPS
## emits the asm, then it is assembled, linked with start_mips.o, and run.
## -G 0 disables gcc's small-data ($gp-relative) addressing: the hand-written
## _start sets up no $gp, so all globals must be reached absolutely (non-PIC).
## The runtime is hard-float o32 (the default), so a leading double argument
## arrives in $f12/$f14 and a double result in $f0; the backend's psABI emits
## exactly that at the boundary.
## Pin the o32 model the skj MIPS backend emits, so the runtime links with
## backend output under either cross. -mfp32 is the 32-bit FP register model
## -march=mips1 implies (the bare-metal mipsel-none-elf cross defaults to it,
## the Debian mipsel-linux-gnu cross rejects -march=mips1 without it).
## -mno-abicalls -fno-pic is the non-PIC, no-$gp model the backend and crt
## use; the linux cross defaults to -mabicalls (SVR4 PIC), which links as
## "abicalls files with non-abicalls files" and faults at runtime.
MIPS_RT_CFLAGS := -march=mips1 -mfp32 -mno-abicalls -fno-pic -EL -G 0 \
                  -std=c99 -O2 -Wall -ffreestanding
MIPS_CC ?= mipsel-none-elf-gcc

build/mips/start_rt.o: runtime/start_mips.S | build/mips
	$(MIPS_AS) -march=mips1 -EL -o $@ $<

## gcc-built runtime objects (o32).  Like the RISC-V build/rv/%.o rule, this
## coexists with build/mips/%.o: build/mips/%.s: make picks the .c rule only
## when no matching .s can be produced (utf8, soft64,
## half have no test source).
build/mips/%.o: runtime/%.c | build/mips
	$(MIPS_CC) $(MIPS_RT_CFLAGS) -c -o $@ $<

## Divide-only soft64 for the libc, as on RISC-V: start_mips.S already has the
## mul and shift family, so a full soft64.o would collide on them.
build/mips/soft64_div.o: runtime/soft64.c | build/mips
	$(MIPS_CC) $(MIPS_RT_CFLAGS) -DSOFT64_NO_MULSHIFT -c -o $@ $<

## I64 integration test
I64_TEST_MIPS_SRC := tests/test_i64.c $(IR_SRC) $(BE_MIPS)
build/test_i64_mips: $(I64_TEST_MIPS_SRC) ir/ir.h | build
	$(CC) $(CFLAGS) -Iir -o $@ $(I64_TEST_MIPS_SRC)
build/mips/i64_test.s: build/test_i64_mips | build/mips
	./build/test_i64_mips -o $@
build/mips/i64_test.o: build/mips/i64_test.s
	$(MIPS_AS) -march=mips1 -EL -o $@ $<
build/mips/i64_test: build/mips/i64_test.o build/mips/start_rt.o
	$(MIPS_LD) -o $@ build/mips/start_rt.o $<
test-i64-mips: build/mips/i64_test
	@echo "Running I64 test under qemu-mipsel..."
	@$(QEMU_MIPS) ./build/mips/i64_test && echo "PASS: i64_test (mips)" || (echo "FAIL: i64_test (mips)"; exit 1)

## Unsigned 32-bit ops integration test
OPS_TEST_MIPS_SRC := tests/test_ops.c $(IR_SRC) $(BE_MIPS)
build/test_ops_mips: $(OPS_TEST_MIPS_SRC) ir/ir.h | build
	$(CC) $(CFLAGS) -Iir -o $@ $(OPS_TEST_MIPS_SRC)
build/mips/ops_test.s: build/test_ops_mips | build/mips
	./build/test_ops_mips -o $@
build/mips/ops_test.o: build/mips/ops_test.s
	$(MIPS_AS) -march=mips1 -EL -o $@ $<
build/mips/ops_test: build/mips/ops_test.o build/mips/start_rt.o
	$(MIPS_LD) -o $@ build/mips/start_rt.o $<
test-ops-mips: build/mips/ops_test
	@echo "Running unsigned ops test under qemu-mipsel..."
	@$(QEMU_MIPS) ./build/mips/ops_test && echo "PASS: ops_test (mips)" || (echo "FAIL: ops_test (mips)"; exit 1)

## FPU integration test (hardware coprocessor 1; links half.o for FLH/FSH,
## since MIPS I has no hardware half-float)
FPU_TEST_MIPS_SRC := tests/test_fpu.c $(IR_SRC) $(BE_MIPS)
build/test_fpu_mips: $(FPU_TEST_MIPS_SRC) ir/ir.h | build
	$(CC) $(CFLAGS) -Iir -o $@ $(FPU_TEST_MIPS_SRC)
build/mips/fpu_test.s: build/test_fpu_mips | build/mips
	./build/test_fpu_mips -o $@
build/mips/fpu_test.o: build/mips/fpu_test.s
	$(MIPS_AS) -march=mips1 -EL -o $@ $<
build/mips/fpu_test: build/mips/fpu_test.o build/mips/start_rt.o build/mips/half.o
	$(MIPS_LD) -o $@ build/mips/start_rt.o build/mips/half.o $<
test-fpu-mips: build/mips/fpu_test
	@echo "Running FPU test under qemu-mipsel..."
	@$(QEMU_MIPS) ./build/mips/fpu_test && echo "PASS: fpu_test (mips)" || (echo "FAIL: fpu_test (mips)"; exit 1)

## Native single-precision (IR_F32) integration test
F32_TEST_MIPS_SRC := tests/test_f32.c $(IR_SRC) $(BE_MIPS)
build/test_f32_mips: $(F32_TEST_MIPS_SRC) ir/ir.h | build
	$(CC) $(CFLAGS) -Iir -o $@ $(F32_TEST_MIPS_SRC)
build/mips/f32_test.s: build/test_f32_mips | build/mips
	./build/test_f32_mips -o $@
build/mips/f32_test.o: build/mips/f32_test.s
	$(MIPS_AS) -march=mips1 -EL -o $@ $<
build/mips/f32_test: build/mips/f32_test.o build/mips/start_rt.o
	$(MIPS_LD) -o $@ build/mips/start_rt.o $<
test-f32-mips: build/mips/f32_test
	@echo "Running F32 test under qemu-mipsel..."
	@$(QEMU_MIPS) ./build/mips/f32_test && echo "PASS: f32_test (mips)" || (echo "FAIL: f32_test (mips)"; exit 1)

## ---------------------------------------------------------------
## PlayStation soft-float tier (R3051, no coprocessor 1)
##
## The MIPS backend built with -DMIPS_SOFTFLOAT keeps every double as a 64-bit
## integer bit pattern and every single as a 32-bit one, lowering each float
## op to a runtime/softfloat.c call.  It emits no coprocessor-1 instruction, so
## it is correct on a machine with no FPU (the PS1's R3051) while still running
## under qemu-mipsel (whose FPU simply stays idle).  Three checks:
##   1. test-softfloat: the library against the host's own double, on the host.
##   2. test-fpu/f32-mips-sf: the IR-level float suites through the soft-float
##      backend, run under qemu, plus a guard that no $f register leaked.
##   3. check-cc-mips-sf: the whole C suite compiled soft-float.
## ---------------------------------------------------------------

## Host-only unit test: the softfloat library against the host's hardware
## double as an oracle.  No cross toolchain, no emulator (the test-gc pattern).
build/test_softfloat: tests/test_softfloat.c runtime/softfloat.c | build
	$(CC) $(CFLAGS) -o $@ tests/test_softfloat.c runtime/softfloat.c -lm
test-softfloat: build/test_softfloat
	@./build/test_softfloat

## FPU integration test, soft-float: same IR as test-fpu-mips, but the backend
## emits softfloat.c calls.  Links half.o (FLH/FSH still narrow through the
## half helpers) and softfloat.o.
build/test_fpu_mips_sf: $(FPU_TEST_MIPS_SRC) ir/ir.h | build
	$(CC) $(CFLAGS) -DMIPS_SOFTFLOAT -Iir -o $@ $(FPU_TEST_MIPS_SRC)
build/mips/fpu_test_sf.s: build/test_fpu_mips_sf | build/mips
	./build/test_fpu_mips_sf -o $@
build/mips/fpu_test_sf.o: build/mips/fpu_test_sf.s
	$(MIPS_AS) -march=mips1 -EL -o $@ $<
build/mips/fpu_test_sf: build/mips/fpu_test_sf.o build/mips/start_rt.o build/mips/half.o build/mips/softfloat.o
	$(MIPS_LD) -o $@ build/mips/start_rt.o build/mips/half.o build/mips/softfloat.o $<
test-fpu-mips-sf: build/mips/fpu_test_sf
	@echo "Checking soft-float output uses no coprocessor-1 register..."
	@if grep -qE '\$$f[0-9]' build/mips/fpu_test_sf.s; then \
	    echo "FAIL: a \$$f register leaked into soft-float output"; exit 1; fi
	@echo "Running FPU test (soft-float) under qemu-mipsel..."
	@$(QEMU_MIPS) ./build/mips/fpu_test_sf && echo "PASS: fpu_test (mips soft-float)" || (echo "FAIL: fpu_test (mips soft-float)"; exit 1)

## F32 integration test, soft-float.
build/test_f32_mips_sf: $(F32_TEST_MIPS_SRC) ir/ir.h | build
	$(CC) $(CFLAGS) -DMIPS_SOFTFLOAT -Iir -o $@ $(F32_TEST_MIPS_SRC)
build/mips/f32_test_sf.s: build/test_f32_mips_sf | build/mips
	./build/test_f32_mips_sf -o $@
build/mips/f32_test_sf.o: build/mips/f32_test_sf.s
	$(MIPS_AS) -march=mips1 -EL -o $@ $<
build/mips/f32_test_sf: build/mips/f32_test_sf.o build/mips/start_rt.o build/mips/half.o build/mips/softfloat.o
	$(MIPS_LD) -o $@ build/mips/start_rt.o build/mips/half.o build/mips/softfloat.o $<
test-f32-mips-sf: build/mips/f32_test_sf
	@echo "Checking soft-float output uses no coprocessor-1 register..."
	@if grep -qE '\$$f[0-9]' build/mips/f32_test_sf.s; then \
	    echo "FAIL: a \$$f register leaked into soft-float output"; exit 1; fi
	@echo "Running F32 test (soft-float) under qemu-mipsel..."
	@$(QEMU_MIPS) ./build/mips/f32_test_sf && echo "PASS: f32_test (mips soft-float)" || (echo "FAIL: f32_test (mips soft-float)"; exit 1)

## ---------------------------------------------------------------
## Cross-backend single-precision differential test (f32 determinism)
##
## The plain test-f32-* suites prove each backend computes single
## precision for *exact* integer-valued cases.  They do NOT prove the
## backends round identically on inexact, subnormal, NaN or conversion
## cases, which is what a game syncing f32 state across two peers on
## different backends needs.  This suite does.
##
## The oracle is the RV32 emulator's own f32 core (emu/rv32.c), already
## verified bit-for-bit against qemu; f32_oracle.c drives it over the
## vector table (tests/f32_vectors.h) and freezes the results as the
## golden master tests/f32_golden.inc (committed).  test-f32-diff-<backend>
## compiles tests/f32_diff.c with that backend's skj-cc, runs it under the
## backend's qemu, and checks every vector against the golden.  A
## divergence names the first offending vector.
build/test_f32_oracle: tests/f32_oracle.c tests/f32_vectors.h emu/rv32.c emu/rv32.h | build
	$(CC) $(CFLAGS) -Iemu -Itests -o $@ tests/f32_oracle.c emu/rv32.c -lm

## The x86-64 byte encoder (jit/emit_x86.c) checked byte for byte against the
## host GNU `as` (intel mode) via objcopy, the golden-master counterpart of
## check-rvas / check-mipsas.  Needs binutils (as, objcopy); no IR or backend is
## involved.  Named test_* so `make check`'s run-tests.sh skips it (it is a host
## tool, not a qemu-m68k guest), the same guard build/test_f32_oracle relies on.
build/test_x86_oracle: tests/x86_oracle.c jit/emit_x86.c jit/code.c jit/emit_x86.h jit/code.h | build
	$(CC) $(CFLAGS) -Ijit -o $@ tests/x86_oracle.c jit/emit_x86.c jit/code.c

check-x86-oracle: build/test_x86_oracle
	@./build/test_x86_oracle

## The AArch64 byte encoder (jit/emit_arm64.c) checked byte for byte against
## aarch64-linux-gnu-as, the golden-master counterpart of check-x86-oracle.
## Needs the arm64 binutils (already an arm64 prerequisite); it assembles on
## the host, so no qemu is involved.  Named test_* so run-tests.sh skips it.
build/test_arm64_oracle: tests/arm64_oracle.c jit/emit_arm64.c jit/code.c jit/emit_arm64.h jit/code.h | build
	$(CC) $(CFLAGS) -Ijit -o $@ tests/arm64_oracle.c jit/emit_arm64.c jit/code.c

check-arm64-oracle: build/test_arm64_oracle
	@./build/test_arm64_oracle aarch64-linux-gnu-as aarch64-linux-gnu-objcopy

## The RV32 byte encoder golden-master (host-only): every jit/emit_rv.c encoder
## checked byte for byte against riscv64-linux-gnu-as, the counterpart of
## check-arm64-oracle.  Runs on the host, no qemu.
build/test_rv_oracle: tests/rv_oracle.c jit/emit_rv.c jit/code.c jit/emit_rv.h jit/code.h | build
	$(CC) $(CFLAGS) -Ijit -o $@ tests/rv_oracle.c jit/emit_rv.c jit/code.c

check-rv-oracle: build/test_rv_oracle
	@./build/test_rv_oracle $(RV_AS) riscv64-linux-gnu-objcopy

## The guest runtime arena (host-only): lazy mapping, word-aligned bump
## allocation, and clean exhaustion.  Built -DSKJ_LP64=1 (the region can live
## anywhere).  Runs on the host, no qemu.
build/test_jit_arena: tests/jit_arena.c jit/jit_arena.c jit/jit_arena.h ir/target.h | build
	$(CC) $(CFLAGS) -DSKJ_LP64=1 -Ijit -Iir -o $@ tests/jit_arena.c jit/jit_arena.c

test-jit-arena: build/test_jit_arena
	@./build/test_jit_arena

## The shared fs/ mount-table + namespace layer (host-only, no qemu): path
## resolution, DOS current-mount + per-mount-cwd state, the ramfs and hostfs
## backends, and hostfs confinement.  Driven directly from C.
FS_SRC := fs/fs.c fs/ramfs.c fs/hostfs.c
FS_HDR := fs/fs.h fs/fs_internal.h fs/ramfs.h fs/hostfs.h fs/ambientfs.h fs/hostcap.h

build/test_fs: tests/fs_test.c $(FS_SRC) $(FS_HDR) | build
	$(CC) $(CFLAGS) -DFS_WITH_HOSTFS -Ifs -o $@ tests/fs_test.c $(FS_SRC)

test-fs: build/test_fs
	@./build/test_fs

## The freestanding (guest) build of the fs/ layer: fs.c + ramfs.c + ambientfs.c.
## hostfs is the hosted/dev backend (host libc DIR*/fstatat); the guest's host
## backend is ambientfs, the temporary AT_FDCWD passthrough compiled with the
## cross toolchain against the guest libc headers (FS_WITH_AMBIENT selects it for
## a FS_RAWPATH host mount).  A real host tree is reached through hostcap, the
## confined backend over a launcher-granted capability fd (FS_WITH_HOSTCAP, the
## default for a host mount).  The default landing spot is now the scratch ramfs;
## FS_AMBIENT_DEFAULT is the documented temporary override that lands a no-mount
## guest on the ambient host instead, kept while the freestanding test harnesses
## still open host files by bare path under qemu.
FS_FREE_CC := ./build/skj-cc-rv -Iinclude -Ilibc -Ifs -DFS_WITH_AMBIENT -DFS_WITH_HOSTCAP -DFS_AMBIENT_DEFAULT
build/kpfree-rv/fs.o: fs/fs.c $(FS_HDR) $(wildcard include/*.h) build/skj-cc-rv build/skj-as-rv Makefile | build/kpfree-rv
	$(FS_FREE_CC) -o $(@:.o=.s) fs/fs.c
	./build/skj-as-rv -o $@ $(@:.o=.s)
build/kpfree-rv/ramfs.o: fs/ramfs.c $(FS_HDR) $(wildcard include/*.h) build/skj-cc-rv build/skj-as-rv Makefile | build/kpfree-rv
	$(FS_FREE_CC) -o $(@:.o=.s) fs/ramfs.c
	./build/skj-as-rv -o $@ $(@:.o=.s)
build/kpfree-rv/ambientfs.o: fs/ambientfs.c $(FS_HDR) $(wildcard include/*.h) build/skj-cc-rv build/skj-as-rv Makefile | build/kpfree-rv
	$(FS_FREE_CC) -o $(@:.o=.s) fs/ambientfs.c
	./build/skj-as-rv -o $@ $(@:.o=.s)
build/kpfree-rv/hostcap.o: fs/hostcap.c $(FS_HDR) $(wildcard include/*.h) build/skj-cc-rv build/skj-as-rv Makefile | build/kpfree-rv
	$(FS_FREE_CC) -o $(@:.o=.s) fs/hostcap.c
	./build/skj-as-rv -o $@ $(@:.o=.s)

check-fs-free: build/kpfree-rv/fs.o build/kpfree-rv/ramfs.o build/kpfree-rv/ambientfs.o build/kpfree-rv/hostcap.o
	@echo "PASS: fs/ (fs.c + ramfs.c + ambientfs.c + hostcap.c) builds freestanding for rv32"

## Regenerate the committed golden master and the index (run after editing
## tests/f32_vectors.h, then commit tests/f32_golden.inc).
f32-oracle: build/test_f32_oracle
	@./build/test_f32_oracle tests/f32_golden.inc build/f32_index.txt

## The human-readable index (vector number -> description + golden value),
## used by the runner to name a diverging vector.  Always regenerated from
## the current oracle so it tracks the vector table.
build/f32_index.txt: build/test_f32_oracle tests/f32_vectors.h
	@./build/test_f32_oracle build/f32_golden_check.inc build/f32_index.txt

## Fail if the committed golden is stale relative to the vector table.  Runs
## the oracle to a scratch file and compares, so it never depends on a
## side-effect of another rule.
f32-golden-check: build/test_f32_oracle tests/f32_vectors.h
	@./build/test_f32_oracle build/f32_golden_check.inc build/f32_index.txt
	@cmp -s build/f32_golden_check.inc tests/f32_golden.inc \
	    && echo "PASS: f32 golden is current" \
	    || (echo "FAIL: tests/f32_golden.inc is stale; run 'make f32-oracle'"; exit 1)

F32_DIFF_DEPS := tests/f32_diff.c tests/f32_golden.inc build/f32_index.txt

test-f32-diff-rv: build/skj-cc-rv build/rv/start.o $(F32_DIFF_DEPS)
	@CCBIN="$(CURDIR)/build/skj-cc-rv" \
	 ASM="$(RV_AS) -march=rv32imfd_zfh -mabi=ilp32 -o" \
	 LD="$(RV_LD) -m elf32lriscv -o" \
	 QEMU="$(QEMU_RV)" START="$(CURDIR)/build/rv/start.o" \
	 OUTDIR="$(CURDIR)/build/f32diff-rv" LABEL="rv32" \
	 sh tests/run-f32-diff.sh

## Native-FPU backends (x86-64, arm64, mips) diverge from the deterministic
## RV32 core on inherent IEEE-underspecified behavior (NaN sign and payload
## quieting), and they do not even agree with each other.  These are XFAIL:
## the divergence is reported for the record, but only a codegen regression
## in the shared f32 core (which would break many vectors) is a hard error
## via test-f32-diff-rv.  Only peers all running the RV32 emulator (smolmoo's
## model) are a verified f32 sync set.
test-f32-diff-x86-64: build/skj-cc-x86-64 build/x86_64/start_sysv.o $(F32_DIFF_DEPS)
	@CCBIN="$(CURDIR)/build/skj-cc-x86-64" \
	 ASM="$(X64_ASM) $(X64_ASMFLAGS) -o" LD="$(X64_LD) -m elf_x86_64 -o" \
	 QEMU="$(QEMU_X64)" START="$(CURDIR)/build/x86_64/start_sysv.o" \
	 OUTDIR="$(CURDIR)/build/f32diff-x86_64" LABEL="x86-64" XFAIL=1 \
	 sh tests/run-f32-diff.sh

test-f32-diff-arm64: build/skj-cc-arm64 build/arm64/start_aapcs.o $(F32_DIFF_DEPS)
	@CCBIN="$(CURDIR)/build/skj-cc-arm64" \
	 ASM="$(A64_AS) -o" LD="$(A64_LD) -o" \
	 QEMU="$(QEMU_A64)" START="$(CURDIR)/build/arm64/start_aapcs.o" \
	 OUTDIR="$(CURDIR)/build/f32diff-arm64" LABEL="arm64" XFAIL=1 \
	 sh tests/run-f32-diff.sh

test-f32-diff-mips: build/skj-cc-mips build/mips/start.o $(F32_DIFF_DEPS)
	@CCBIN="$(CURDIR)/build/skj-cc-mips" \
	 ASM="$(MIPS_AS) -march=mips1 -EL -o" LD="$(MIPS_LD) -o" \
	 QEMU="$(QEMU_MIPS)" START="$(CURDIR)/build/mips/start.o" \
	 OUTDIR="$(CURDIR)/build/f32diff-mips" LABEL="mips" XFAIL=1 \
	 sh tests/run-f32-diff.sh

## ColdFire's FPU computes in a wider internal format and cannot hold a
## bit-exact binary32 at all; like the other native-FPU backends it is not
## a verified f32 sync peer, so its divergences are XFAIL.
test-f32-diff-cf: build/skj-cc build/skj-as build/start.o $(F32_DIFF_DEPS)
	@CCBIN="$(CURDIR)/build/skj-cc" \
	 ASM="$(CURDIR)/build/skj-as -o" LD="$(M68K_LD) -o" \
	 QEMU="$(QEMU)" START="$(CURDIR)/build/start.o" \
	 OUTDIR="$(CURDIR)/build/f32diff-cf" LABEL="coldfire" XFAIL=1 \
	 sh tests/run-f32-diff.sh

## The full matrix.  RV is the reference and must pass; the others surface
## whatever f32 divergences exist between them and the deterministic core.
test-f32-diff: f32-golden-check test-f32-diff-rv test-f32-diff-x86-64 \
               test-f32-diff-arm64 test-f32-diff-mips test-f32-diff-cf
	@echo "f32 differential matrix complete."

## ---------------------------------------------------------------
## Cross-backend differential torture (V.E)
##
## One adversarial program is fed to every backend and runner, and all peers
## must produce byte-identical stdout and the same exit status.  Differential
## execution is the oracle: there are no golden files, so a divergence is a
## real disagreement between two code paths for one source.  Peers whose cross
## toolchain or qemu is absent are skipped; the comparison runs among those
## present (at least two are needed).  Integer, 64-bit, and control-flow only;
## float is out of scope here (see test-f32-diff).  Opt-in: it needs several
## cross assemblers, linkers, and qemu user-mode binaries.
TORTURE_DIFF_DEPS := build/skj-cc build/skj-cc-rv build/skj-cc-x86-64 \
    build/skj-cc-arm64 build/skj-cc-mips build/skj-jit build/skj-run \
    build/start.o build/rv/start.o build/x86_64/start_sysv.o \
    build/arm64/start_aapcs.o build/mips/start.o

test-torture-diff: $(TORTURE_DIFF_DEPS)
	@GCC="$(CC)" M68K_AS="$(M68K_AS)" M68K_LD="$(M68K_LD)" QEMU="$(QEMU)" \
	 RV_AS="$(RV_AS)" RV_LD="$(RV_LD)" QEMU_RV="$(QEMU_RV)" \
	 X64_ASM="$(X64_ASM)" X64_ASMFLAGS="$(X64_ASMFLAGS)" X64_LD="$(X64_LD)" QEMU_X64="$(QEMU_X64)" \
	 A64_AS="$(A64_AS)" A64_LD="$(A64_LD)" QEMU_A64="$(QEMU_A64)" \
	 MIPS_AS="$(MIPS_AS)" MIPS_LD="$(MIPS_LD)" QEMU_MIPS="$(QEMU_MIPS)" \
	 sh tests/run-torture-diff.sh

## ---------------------------------------------------------------
## The strict-ColdFire tier: the same tests on the in-tree emulator
##
## `make check` cross-compiles the C runtime for generic m68k, which is
## what qemu-m68k models.  gcc then emits 68020 instructions (64-bit
## divide, byte and word arithmetic on memory) that neither a real
## ColdFire nor skj-run has, so the emulator tier rebuilds the runtime
## with -mcpu=5475 and relinks into build/cf.  The compiled test objects
## are already ColdFire, so they are reused as they are; only the
## gcc-built runtime differs.  This is the same code a boris or smolmoo
## host would load, so the emulator tier is the closer model of the real
## target and the qemu tier stays as an independent oracle.
## ---------------------------------------------------------------
CF_RT_CFLAGS := -std=c99 -O2 -Wall -ffreestanding -mcpu=5475

build/cf:
	mkdir -p build/cf

build/cf/%.o: runtime/%.c | build/cf
	$(M68K_CC) $(CF_RT_CFLAGS) -c -o $@ $<

CF_MOO_RT     := build/start.o build/cf/str.o build/cf/list.o build/cf/host_stub.o
CF_MOO_TOY_RT := build/start.o build/cf/str.o build/cf/list.o build/cf/toy_host.o

CF_TC_BINS  := $(TESTS:tests/%.tc=build/cf/%)
CF_SCM_BINS := $(SCM_TESTS:tests/%.scm=build/cf/%)
CF_MOO_BINS := $(MOO_TESTS:tests/%.moo=build/cf/%)
CF_TOY_BINS := $(MOO_TOY_TESTS:tests/%.moo=build/cf/%)
CF_PAS_BINS := $(PAS_TESTS_NOFLOAT:tests/%.pas=build/cf/%)

$(CF_TC_BINS) $(CF_SCM_BINS): build/cf/%: build/%.o build/start.o build/skj-ld | build/cf
	./build/skj-ld -o $@ build/start.o $<

$(CF_MOO_BINS): build/cf/%: build/%.o $(CF_MOO_RT) build/skj-ld | build/cf
	./build/skj-ld -o $@ $(CF_MOO_RT) $<

$(CF_TOY_BINS): build/cf/%: build/%.o $(CF_MOO_TOY_RT) build/skj-ld | build/cf
	./build/skj-ld -o $@ $(CF_MOO_TOY_RT) $<

$(CF_PAS_BINS): build/cf/%: build/%.o build/start.o build/cf/pascal_rt.o build/skj-ld | build/cf
	./build/skj-ld -o $@ build/start.o build/cf/pascal_rt.o $<

check-emu: build/skj-run build/skj-tinc build/skj-mooc build/skj-pc \
           $(CF_TC_BINS) $(CF_SCM_BINS) $(CF_MOO_BINS) $(CF_TOY_BINS) $(CF_PAS_BINS)
	@sh tests/run-tests.sh ./build/skj-run build/cf

## The RISC-V tests link no cross-compiled C, so the same binaries the
## qemu tier runs go straight to the emulator.
check-rv-emu: build/skj-run build/skj-tinc-rv build/skj-sc-rv \
              $(RV_TESTS:tests/%.tc=build/rv/%) $(RV_SCM_TESTS:tests/%.scm=build/rv/%)
	@sh tests/run-tests.sh ./build/skj-run build/rv

## ---------------------------------------------------------------
## Instruction-count benchmark
##
## The emulator counts retired instructions with no timing input, so the
## count for a guest is identical on every host.  That makes it a
## reproducible benchmark metric where wall-clock time is not: tests/bench.sh
## runs the corpus in tests/bench.list under skj-run --stats and compares
## each count against the committed baseline tests/bench.expected.  A change
## in either direction fails, since on a deterministic interpreter the count
## only moves when the generated code does; re-accept with bench-accept once
## the change is intended.  Not part of `check`: it is a regression signal to
## read, and it needs both cross toolchains to cover the whole corpus.  Run
## bench-cf or bench-rv to gate one core when only one toolchain is present.
##
## The corpus reuses the guests check-emu and check-rv-emu already build, so
## these share their prerequisite sets.
BENCH_CF_DEPS := $(CF_TC_BINS) $(CF_SCM_BINS) $(CF_MOO_BINS) $(CF_TOY_BINS) \
                 $(CF_PAS_BINS)
BENCH_RV_DEPS := build/skj-tinc-rv build/skj-sc-rv \
                 $(RV_TESTS:tests/%.tc=build/rv/%) \
                 $(RV_SCM_TESTS:tests/%.scm=build/rv/%)

bench: build/skj-run $(BENCH_CF_DEPS) $(BENCH_RV_DEPS)
	@sh tests/bench.sh ./build/skj-run build tests/bench.list tests/bench.expected

bench-cf: build/skj-run $(BENCH_CF_DEPS)
	@sh tests/bench.sh ./build/skj-run build tests/bench.list tests/bench.expected

bench-rv: build/skj-run $(BENCH_RV_DEPS)
	@sh tests/bench.sh ./build/skj-run build tests/bench.list tests/bench.expected

bench-accept: build/skj-run $(BENCH_CF_DEPS) $(BENCH_RV_DEPS)
	@sh tests/bench.sh ./build/skj-run build tests/bench.list tests/bench.expected --accept

## ---------------------------------------------------------------
## RISC-V platform-ABI runtime objects
##
## The runtime objects are portable C cross-compiled for rv32: soft64.c
## supplies the 64-bit helpers, since the toolchain ships no rv32 libgcc
## to take them from.
## ---------------------------------------------------------------
RV_CC       ?= riscv64-linux-gnu-gcc
RV_AR       ?= riscv64-linux-gnu-ar
RV_ARCH     := -march=rv32imfd -mabi=ilp32
RV_AS_ARCH  := -march=rv32imfd_zfh -mabi=ilp32
RV_RT_CFLAGS := -std=c99 -O2 -Wall -ffreestanding $(RV_ARCH)

build/rv/%.o: runtime/%.c | build/rv
	$(RV_CC) $(RV_RT_CFLAGS) -c -o $@ $<

## Divide-only soft64 for the libc: start_rv.S already defines the mul and
## shift family, so only the __udivdi3 div family must be added (a full
## soft64.o would collide on the shifts).  A separate object from the shared
## build/rv/soft64.o, which the Excelsior runtime links whole.
build/rv/soft64_div.o: runtime/soft64.c | build/rv
	$(RV_CC) $(RV_RT_CFLAGS) -DSOFT64_NO_MULSHIFT -c -o $@ $<

## The IR-builder integration tests on the emulator.  i64 and ops link no
## cross-compiled C, so they run the very binaries the qemu tier runs; the
## FPU test links half.o, which has to be the ColdFire build.
build/cf/fpu_test: build/fpu_test.o build/start.o build/cf/half.o build/skj-ld | build/cf
	./build/skj-ld -o $@ build/start.o build/cf/half.o build/fpu_test.o

test-fpu-emu: build/skj-run build/cf/fpu_test
	@./build/skj-run ./build/cf/fpu_test && echo "PASS: fpu_test (skj-run)" \
		|| (echo "FAIL: fpu_test (skj-run)"; exit 1)

test-i64-emu: build/skj-run build/i64_test_cf
	@./build/skj-run ./build/i64_test_cf && echo "PASS: i64_test (skj-run)" \
		|| (echo "FAIL: i64_test (skj-run)"; exit 1)

test-ops-emu: build/skj-run build/ops_test_cf
	@./build/skj-run ./build/ops_test_cf && echo "PASS: ops_test (skj-run)" \
		|| (echo "FAIL: ops_test (skj-run)"; exit 1)

## Every suite the in-tree emulator can run, no qemu involved.
check-emu-all: check-emu check-rv-emu \
               test-rv-irq test-rv-run test-guest-mmap \
               test-rv-sandbox test-rv-xlang-sandbox test-rv-decode-cache \
               test-rv-expand test-rv-bus test-rv-csr test-rv-decode test-rv-fp \
               check-rv32 \
               test-fpu-emu test-i64-emu test-ops-emu bench \
               check-kp-jit-rv check-kp-jit-arm64 check-kp-aot-rv check-kp-aot-arm64 \
               check-kp-free check-kp-corpus-rv check-kp-corpus-arm64 \
               check-kp-multi-rv check-kp-multi-arm64 \
               check-kp-io-rv check-kp-io-arm64 check-kp-io-free \
               check-kp-inc-rv check-kp-inc-arm64 check-kp-inc-free
	@echo "All emulator test suites passed."

## The host-only emulator tests, rebuilt with ASan+UBSan into build/san so
## they never clobber the plain binaries.  These drive emu/rv32.c and
## emu/guest.c directly; the sandbox reuses a sanitized skj-run so emu/rv_user.c
## is covered too, run against the already-built cross ELF (a guest ELF is
## unaffected by host sanitizers).  Not part of check-emu-all: it needs a
## sanitizer-capable host compiler, so it is opt-in.
build/san:
	mkdir -p build/san

build/san/test_rv_run: tests/test_rv_run.c emu/rv32.c emu/rv32.h | build build/san
	$(CC) $(CFLAGS) $(SAN_CFLAGS) -Iemu -o $@ tests/test_rv_run.c emu/rv32.c -lm

build/san/test_guest_mmap: tests/test_guest_mmap.c emu/guest.c emu/guest.h \
                           emu/hostplat.c emu/hostplat.h emu/elf32.c emu/elf32.h | build build/san
	$(CC) $(CFLAGS) $(SAN_CFLAGS) -Iemu -Ifs -o $@ tests/test_guest_mmap.c \
	    emu/guest.c emu/hostplat.c emu/elf32.c -lm

build/san/test_rv_decode_cache: tests/test_rv_decode_cache.c emu/rv32.c emu/rv32.h | build build/san
	$(CC) $(CFLAGS) $(SAN_CFLAGS) -Iemu -o $@ tests/test_rv_decode_cache.c emu/rv32.c -lm

build/san/test_rv_irq: tests/test_rv_irq.c emu/rv32.c emu/rv32.h | build build/san
	$(CC) $(CFLAGS) $(SAN_CFLAGS) -Iemu -o $@ tests/test_rv_irq.c emu/rv32.c -lm

build/san/test_rv_expand: tests/test_rv_expand.c emu/rv32.c emu/rv32.h | build build/san
	$(CC) $(CFLAGS) $(SAN_CFLAGS) -Iemu -o $@ tests/test_rv_expand.c emu/rv32.c -lm

build/san/test_rv_bus: tests/test_rv_bus.c emu/rv32.c emu/rv32.h | build build/san
	$(CC) $(CFLAGS) $(SAN_CFLAGS) -Iemu -o $@ tests/test_rv_bus.c emu/rv32.c -lm

build/san/test_rv_csr: tests/test_rv_csr.c emu/rv32.c emu/rv32.h | build build/san
	$(CC) $(CFLAGS) $(SAN_CFLAGS) -Iemu -o $@ tests/test_rv_csr.c emu/rv32.c -lm

build/san/test_rv_decode: tests/test_rv_decode.c emu/rv32.c emu/rv32.h | build build/san
	$(CC) $(CFLAGS) $(SAN_CFLAGS) -Iemu -o $@ tests/test_rv_decode.c emu/rv32.c -lm

build/san/test_rv_fp: tests/test_rv_fp.c emu/rv32.c emu/rv32.h | build build/san
	$(CC) $(CFLAGS) $(SAN_CFLAGS) -Iemu -o $@ tests/test_rv_fp.c emu/rv32.c -lm

build/san/skj-run: $(EMU_SRC) $(EMU_HDR) build/version.h | build build/san
	$(CC) $(CFLAGS) $(SAN_CFLAGS) $(EMU_FLAGS) -Iemu -o $@ $(EMU_SRC) -lm

check-emu-san: build/san/test_rv_run build/san/test_guest_mmap \
               build/san/test_rv_decode_cache build/san/test_rv_irq \
               build/san/test_rv_expand build/san/test_rv_bus \
               build/san/test_rv_csr build/san/test_rv_decode build/san/test_rv_fp \
               build/san/skj-run build/sandbox/parent.elf
	@./build/san/test_rv_run
	@./build/san/test_guest_mmap
	@./build/san/test_rv_decode_cache
	@./build/san/test_rv_irq
	@./build/san/test_rv_expand
	@./build/san/test_rv_bus
	@./build/san/test_rv_csr
	@./build/san/test_rv_decode
	@./build/san/test_rv_fp
	@sh tests/run-sandbox-test.sh "$(CURDIR)/build/san/skj-run" "$(CURDIR)/build/sandbox/parent.elf"
	@echo "All host-only emulator tests passed under ASan+UBSan."

## ---------------------------------------------------------------------------
## Workstream A: the compiler toolchain under ASan+UBSan (check-san)
##
## The sanitizer surface used to be emu/ alone (check-emu-san); every code
## generator, assembler, linker, the preprocessor, and the front ends ran
## plain.  check-san rebuilds the host tools with the sanitizers into build/san
## and re-runs the script-driven suites against them, so the existing corpus
## (the cc, cpp, Excelsior, rvas, and kp cases) becomes an ASan+UBSan probe of
## the tools themselves.  The guest binaries still run under qemu unsanitized;
## only the host tools carry instrumentation.
##
## The parse/emit tools and the JIT drivers take full ASan+UBSan.
## ASAN_OPTIONS=detect_leaks=0 because the tools arena-allocate
## and exit without freeing (not a bug here), so LeakSanitizer is off while the
## memory-error and UB checks stay on; -fno-sanitize-recover aborts loudly on a
## real finding, which the suite then reports as a tool failure.

build/san/skj-cpp: cpp/main.c $(CPP_SRC) cpp/cpp.h cpp/internal.h build/version.h | build build/san
	$(CC) $(CFLAGS) $(SAN_CFLAGS) -Icpp -Iir -o $@ cpp/main.c $(CPP_SRC)

build/san/skj-cc: $(CC_SRC) cc/cc.h cpp/cpp.h cpp/internal.h ir/ir.h build/version.h | build build/san
	$(CC) $(CFLAGS) $(SAN_CFLAGS) -DCC_M68K $(SKJ_INC_DEF) -Icc -Icpp -Iir -o $@ $(CC_SRC)

build/san/skj-as: $(AS_SRC) as/as.h as/asm_lex.h as/asm_obj.h dwarf/dwarf.h build/version.h | build build/san
	$(CC) $(CFLAGS) $(SAN_CFLAGS) -Ias -Iir -Idwarf -o $@ $(AS_SRC)

build/san/skj-ld: $(LD_SRC) ld/ld.h ld/mapfile.h build/version.h | build build/san
	$(CC) $(CFLAGS) $(SAN_CFLAGS) -Ild -Iir -o $@ $(LD_SRC)

build/san/skj-as-rv: $(AS_RV_SRC) as/rv.h as/asm_lex.h as/asm_obj.h dwarf/dwarf.h build/version.h | build build/san
	$(CC) $(CFLAGS) $(SAN_CFLAGS) -Ias -Iir -Idwarf -o $@ $(AS_RV_SRC)

build/san/skj-ld-rv: $(LD_RV_SRC) ld/ld.h ld/rv_ld.h ld/mapfile.h build/version.h | build build/san
	$(CC) $(CFLAGS) $(SAN_CFLAGS) -Ild -Iir -o $@ $(LD_RV_SRC)

build/san/skj-kp: kobold/aot_main.c kobold/kpm.c $(IR_SRC) $(BE_X64_SEL) kobold/kpm.h ir/ir.h \
                  backend/mc_text.h backend/x86_select.h build/version.h | build build/san
	$(CC) $(CFLAGS) $(SAN_CFLAGS) -DX86_BITS=64 -Iir -Ibackend -Ikobold -o $@ \
	    kobold/aot_main.c kobold/kpm.c $(IR_SRC) $(BE_X64_SEL)

# The KPM1 reader-pipeline fuzz harness (kpm_read -> regalloc -> target_emit).
# This path maps no executable memory, so full ASan applies beside UBSan.
build/san/kpm-fuzz: kobold/kpm_fuzz.c kobold/kpm.c $(IR_SRC) $(BE_X64_SEL) kobold/kpm.h ir/ir.h \
                    backend/mc_text.h backend/x86_select.h tests/fuzz_mutate.h build/version.h | build build/san
	$(CC) $(CFLAGS) $(SAN_CFLAGS) -DX86_BITS=64 -Iir -Ibackend -Ikobold -Itests -o $@ \
	    kobold/kpm_fuzz.c kobold/kpm.c $(IR_SRC) $(BE_X64_SEL)

# The preprocessor fuzz harness.  Pure text in, text out, no executable memory,
# so full ASan applies.  -D_GNU_SOURCE exposes mkstemp under -std=c99.
build/san/cpp-fuzz: cpp/cpp_fuzz.c $(CPP_SRC) cpp/cpp.h cpp/internal.h ir/util.h \
                    tests/fuzz_mutate.h build/version.h | build build/san
	$(CC) $(CFLAGS) $(SAN_CFLAGS) -D_GNU_SOURCE -Icpp -Iir -Itests -o $@ \
	    cpp/cpp_fuzz.c $(CPP_SRC)

# The assembler fuzz harnesses (one arch-parameterized source over each
# assembler's library, main.c dropped).  Lexer/parser/encoder, text in, object
# out (discarded), so full ASan applies.
AS_FUZZ_LIB    := $(filter-out as/main.c,$(AS_SRC))
AS_RV_FUZZ_LIB := $(filter-out as/rv_main.c,$(AS_RV_SRC))
AS_MIPS_FUZZ_LIB := $(filter-out as/mips_main.c,$(AS_MIPS_SRC))
AS_X64_FUZZ_LIB := $(filter-out as/x86_64_main.c,$(AS_X64_SRC))
AS_A64_FUZZ_LIB := $(filter-out as/aarch64_main.c,$(AS_A64_SRC))

build/san/as-fuzz: as/as_fuzz.c $(AS_FUZZ_LIB) as/as.h as/asm_lex.h as/asm_obj.h \
                   tests/fuzz_mutate.h build/version.h | build build/san
	$(CC) $(CFLAGS) $(SAN_CFLAGS) -D_GNU_SOURCE -DASFUZZ_CF -Ias -Iir -Idwarf -Itests -o $@ \
	    as/as_fuzz.c $(AS_FUZZ_LIB)

build/san/as-rv-fuzz: as/as_fuzz.c $(AS_RV_FUZZ_LIB) as/rv.h as/asm_lex.h as/asm_obj.h \
                      dwarf/dwarf.h tests/fuzz_mutate.h build/version.h | build build/san
	$(CC) $(CFLAGS) $(SAN_CFLAGS) -D_GNU_SOURCE -DASFUZZ_RV -Ias -Iir -Idwarf -Itests -o $@ \
	    as/as_fuzz.c $(AS_RV_FUZZ_LIB)

build/san/as-mips-fuzz: as/as_fuzz.c $(AS_MIPS_FUZZ_LIB) as/mips.h as/asm_lex.h as/asm_obj.h \
                        tests/fuzz_mutate.h build/version.h | build build/san
	$(CC) $(CFLAGS) $(SAN_CFLAGS) -D_GNU_SOURCE -DASFUZZ_MIPS -Ias -Iir -Idwarf -Itests -o $@ \
	    as/as_fuzz.c $(AS_MIPS_FUZZ_LIB)

build/san/as-x86-64-fuzz: as/as_fuzz.c $(AS_X64_FUZZ_LIB) as/x86_64.h as/asm_lex.h as/asm_obj.h \
                          tests/fuzz_mutate.h build/version.h | build build/san
	$(CC) $(CFLAGS) $(SAN_CFLAGS) -D_GNU_SOURCE -DASFUZZ_X86_64 -Ias -Iir -Itests -o $@ \
	    as/as_fuzz.c $(AS_X64_FUZZ_LIB)

build/san/as-aarch64-fuzz: as/as_fuzz.c $(AS_A64_FUZZ_LIB) as/aarch64.h as/asm_lex.h as/asm_obj.h \
                           tests/fuzz_mutate.h build/version.h | build build/san
	$(CC) $(CFLAGS) $(SAN_CFLAGS) -D_GNU_SOURCE -DASFUZZ_AARCH64 -Ias -Iir -Itests -o $@ \
	    as/as_fuzz.c $(AS_A64_FUZZ_LIB)

# The IR front-end parser fuzz harnesses (one arch-parameterized source over
# each front end's library, main.c dropped).  Lexer + parser + lowering, source
# in, assembly out (discarded), so full ASan applies.  MOO_SRC and PC_SRC are
# defined later than this rule, so the pascal and moo prerequisites list the
# sources explicitly (the deferred-definition hazard noted on the san rules);
# SRC (tinc) is defined earlier and used directly.
FE_TINC_LIB := $(filter-out tinc/main.c,$(SRC))

build/san/fe-tinc-fuzz: tests/fe_fuzz.c $(FE_TINC_LIB) tinc/tinc.h ir/ir.h \
                        tests/fuzz_mutate.h build/version.h | build build/san
	$(CC) $(CFLAGS) $(SAN_CFLAGS) -D_GNU_SOURCE -DFEFUZZ_TINC -Itinc -Iir -Itests -o $@ \
	    tests/fe_fuzz.c $(FE_TINC_LIB)

build/san/fe-pascal-fuzz: tests/fe_fuzz.c pascal/lex.c pascal/parse.c pascal/lower.c \
                          $(IR_SRC) $(BE_CF) pascal/pascal.h ir/ir.h \
                          tests/fuzz_mutate.h build/version.h | build build/san
	$(CC) $(CFLAGS) $(SAN_CFLAGS) -D_GNU_SOURCE -DFEFUZZ_PASCAL -Ipascal -Iir -Itests -o $@ \
	    tests/fe_fuzz.c pascal/lex.c pascal/parse.c pascal/lower.c $(IR_SRC) $(BE_CF)

build/san/fe-moo-fuzz: tests/fe_fuzz.c moo/lex.c moo/parse.c moo/typecheck.c moo/lower.c \
                       $(IR_SRC) $(BE_CF) moo/moo.h ir/ir.h \
                       tests/fuzz_mutate.h build/version.h | build build/san
	$(CC) $(CFLAGS) $(SAN_CFLAGS) -D_GNU_SOURCE -DFEFUZZ_MOO -Imoo -Iir -Itests -o $@ \
	    tests/fe_fuzz.c moo/lex.c moo/parse.c moo/typecheck.c moo/lower.c $(IR_SRC) $(BE_CF)

# The cc harness drives lex -> parse -> lower; the real driver preprocesses
# first, but the preprocessor is fuzzed on its own, so this feeds source
# straight to the parser (the one cc test with a # directive dies early in the
# lexer, which is fine).  -DCC_M68K matches the shipping skj-cc type model.
build/san/fe-cc-fuzz: tests/fe_fuzz.c cc/lex.c cc/parse.c cc/type.c cc/lower.c \
                      $(IR_SRC) $(BE_CF) cc/cc.h ir/ir.h \
                      tests/fuzz_mutate.h build/version.h | build build/san
	$(CC) $(CFLAGS) $(SAN_CFLAGS) -D_GNU_SOURCE -DCC_M68K -DFEFUZZ_CC -Icc -Iir -Itests -o $@ \
	    tests/fe_fuzz.c cc/lex.c cc/parse.c cc/type.c cc/lower.c $(IR_SRC) $(BE_CF)

build/san/skj-jit: $(JIT_SRC) cc/cc.h jit/jit_common.h jit/jit_arena.h jit/jit_x86.h jit/emit_x86.h jit/code.h backend/mc.h backend/x86_select.h ir/ir.h build/version.h | build build/san
	$(CC) $(CFLAGS) $(SAN_CFLAGS) -DX86_BITS=64 -DSKJ_LP64=1 -DCC_PSABI -DCC_STRUCT_ABI $(SKJ_INC_DEF) -Icc -Icpp -Iir -Ijit -Ibackend -o $@ $(JIT_SRC)

# The remaining front ends (tinc, scheme, moo, Pascal) and the archive tool
# also take full ASan+UBSan; each is probed compile-only by run-compile-san.sh.
# SC_SRC,
# MOO_SRC, and PC_SRC are defined later in the Makefile than these rules, so a
# prerequisite of them would expand empty at read time and never trigger a
# rebuild (a deferred-definition hazard);
# the source files are listed explicitly in the prerequisites here, while the
# recipe's deferred $(VAR) expansion is fine.  SRC and AR_SRC are defined
# before this point, so those rules use the variable directly.
build/san/skj-tinc: $(SRC) ir/ir.h tinc/tinc.h build/version.h | build build/san
	$(CC) $(CFLAGS) $(SAN_CFLAGS) -Iir -Itinc -o $@ $(SRC)

build/san/skj-sc: scheme/lex.c scheme/parse.c scheme/print.c scheme/main.c \
                  scheme/gc.c scheme/lower.c $(IR_SRC) $(BE_CF) \
                  scheme/scheme.h scheme/gc.h ir/ir.h build/version.h | build build/san
	$(CC) $(CFLAGS) $(SAN_CFLAGS) -Ischeme -Iir -o $@ $(SC_SRC)

build/san/skj-mooc: moo/lex.c moo/parse.c moo/typecheck.c moo/lower.c moo/main.c \
                    $(IR_SRC) $(BE_CF) moo/moo.h ir/ir.h build/version.h | build build/san
	$(CC) $(CFLAGS) $(SAN_CFLAGS) -Imoo -Iir -o $@ $(MOO_SRC)

build/san/skj-pc: pascal/lex.c pascal/parse.c pascal/lower.c pascal/main.c \
                  $(IR_SRC) $(BE_CF) pascal/pascal.h ir/ir.h build/version.h | build build/san
	$(CC) $(CFLAGS) $(SAN_CFLAGS) -Ipascal -Iir -o $@ $(PC_SRC)

build/san/skj-ar: $(AR_SRC) ld/mapfile.h ir/util.h ir/arena.h build/version.h | build build/san
	$(CC) $(CFLAGS) $(SAN_CFLAGS) -Ild -Iir -o $@ $(AR_SRC)

# The Kobold behavioral corpora run under full ASan+UBSan, LP64 to match the
# shipped host so cross-module .kpm loads width-check clean.  kp_co_lp64.o stays
# unsanitized (raw stack-switch coroutine assembly); the sanitizers are
# per-TU, so linking it into a sanitized binary is fine.
build/san/skj-kp-jit: kobold/jit_main.c kobold/frontend.c build/x86-64/kpcom_kpm.c $(KP_RT) build/kp_co_lp64.o kobold/frontend.h $(KP_HDRS) build/version.h Makefile | build build/san
	$(CC) $(KP_CFLAGS) -Ibuild/x86-64 $(SAN_CFLAGS) -Ibuild -o $@ kobold/jit_main.c kobold/frontend.c build/x86-64/kpcom_kpm.c $(KP_RT) build/kp_co_lp64.o -lm

## Each sanitized suite is its own target over the build/san tools, so a failing
## slice fails on its own (a single recipe line returns the script's status, no
## masking) and the slices compose into the aggregates below.  SAN_RUNENV sets
## the sanitizer runtime options every slice shares (see the block comment above
## for why detect_leaks is off).
SAN_RUNENV := ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1

check-san-cpp: build/san/skj-cpp
	@echo "== check-san: preprocessor (skj-cpp) =="
	@$(SAN_RUNENV) CPP="$(CURDIR)/build/san/skj-cpp" sh tests/run-cpp-tests.sh

check-san-cc: build/san/skj-cc build/san/skj-as build/start.o build/half.o build/va_cf.o
	@echo "== check-san: C compiler (skj-cc + skj-as, ColdFire) =="
	@$(SAN_RUNENV) CCBIN="$(CURDIR)/build/san/skj-cc" ASM="$(CURDIR)/build/san/skj-as -o" \
	   OUTDIR="$(CURDIR)/build/san/cc-out" EXCLUDE="$(CF_EXCLUDE)" \
	   HALF="$(CURDIR)/build/half.o $(CURDIR)/build/va_cf.o" \
	   sh tests/run-cc-tests.sh

check-san-rvas: build/san/skj-as-rv build/san/skj-ld-rv build/skj-run
	@echo "== check-san: RV32 assembler/linker (skj-as-rv + skj-ld-rv) =="
	@$(SAN_RUNENV) SKJ="$(CURDIR)/build/san/skj-as-rv" SKJLD="$(CURDIR)/build/san/skj-ld-rv" \
	   RUN="$(CURDIR)/build/skj-run" GAS="$(RV_AS)" GLD="$(RV_LD)" QEMU="$(QEMU_RV)" \
	   sh tests/run-rvas-tests.sh

check-san-kp: build/san/skj-kp
	@echo "== check-san: KPM1 reader (skj-kp) =="
	@$(SAN_RUNENV) KP="$(CURDIR)/build/san/skj-kp" sh tests/run-kp-tests.sh

check-san-jit: build/san/skj-jit
	@echo "== check-san: C via the JIT (skj-jit, ASan+UBSan) =="
	@$(SAN_RUNENV) JIT="$(CURDIR)/build/san/skj-jit" sh tests/run-jit-tests.sh

check-san-tinc: build/san/skj-tinc
	@echo "== check-san: TinC front end (skj-tinc, compile-only) =="
	@$(SAN_RUNENV) sh tests/run-compile-san.sh "$(CURDIR)/build/san/skj-tinc" tinc $(TESTS)

check-san-scheme: build/san/skj-sc
	@echo "== check-san: TinScheme front end (skj-sc, compile-only) =="
	@$(SAN_RUNENV) sh tests/run-compile-san.sh "$(CURDIR)/build/san/skj-sc" scheme $(SCM_TESTS)

check-san-moo: build/san/skj-mooc
	@echo "== check-san: MooScript front end (skj-mooc, compile-only) =="
	@$(SAN_RUNENV) sh tests/run-compile-san.sh "$(CURDIR)/build/san/skj-mooc" moo $(MOO_TESTS) $(MOO_TOY_TESTS)

check-san-pc: build/san/skj-pc
	@echo "== check-san: Compact Pascal front end (skj-pc, compile-only) =="
	@$(SAN_RUNENV) sh tests/run-compile-san.sh "$(CURDIR)/build/san/skj-pc" pascal $(PAS_TESTS)

check-san-ar: build/san/skj-ar build/start.o build/half.o
	@echo "== check-san: archive tool (skj-ar) =="
	@$(SAN_RUNENV) AR="$(CURDIR)/build/san/skj-ar" sh tests/run-ar-san.sh \
	   build/start.o build/half.o

## The Kobold behavioral corpora (the hosted corpus and the multi-unit set)
## through the ASan+UBSan skj-kp-jit, run in JIT mode so the sanitized driver
## both compiles and executes each program.
check-san-kp-corpus: build/san/skj-kp-jit
	@echo "== check-san: Kobold hosted corpus (skj-kp-jit, ASan+UBSan) =="
	@$(SAN_RUNENV) QEMU= KP_SRCDIR=tests/kp-corpus MODE=jit JIT="$(CURDIR)/build/san/skj-kp-jit" \
	   sh tests/run-kp-src-tests.sh

check-san-kp-multi: build/san/skj-kp-jit
	@echo "== check-san: Kobold multi-unit corpus (skj-kp-jit, ASan+UBSan) =="
	@$(SAN_RUNENV) QEMU= KP_SRCDIR=tests/kp-multi MODE=jit JIT="$(CURDIR)/build/san/skj-kp-jit" \
	   sh tests/run-kp-multi-tests.sh

check-san-kp-io: build/san/skj-kp-jit
	@echo "== check-san: Kobold io corpus (skj-kp-jit, ASan+UBSan) =="
	@$(SAN_RUNENV) QEMU= KP_SRCDIR=tests/kp-io MODE=jit JIT="$(CURDIR)/build/san/skj-kp-jit" \
	   sh tests/run-kp-src-tests.sh

check-san-kp-inc: build/san/skj-kp-jit
	@echo "== check-san: Kobold include corpus (skj-kp-jit, ASan+UBSan) =="
	@$(SAN_RUNENV) QEMU= KP_SRCDIR=tests/kp-inc MODE=jit JIT="$(CURDIR)/build/san/skj-kp-jit" \
	   sh tests/run-kp-src-tests.sh

## The KPM1 reader-pipeline fuzz: mutate valid seed modules and run each result
## through kpm_read -> regalloc -> target_emit under ASan+UBSan.  A clean exit
## means no input, valid or hostile, drove undefined behavior through the
## pipeline.  The PRNG seed is fixed so a run reproduces; KPM_FUZZ_ITERS and
## KPM_FUZZ_RNG override the count and seed.  The default iteration count keeps
## the run to a few seconds; a longer campaign drives build/san/kpm-fuzz from
## stdin with AFL++ or an external byte loop, the same binary without --mutate.
##
## The seeds are the two reader fixtures plus a handful of real corpus modules
## emitted by the front end (skj-kp-jit --emit-kpm), so the mutations reach a
## broad spread of opcodes (arrays, casts, int64, reals, records, coroutines,
## sets, transcendentals) rather than only the fixtures' small op set.
KPM_FUZZ_ITERS ?= 50000
KPM_FUZZ_RNG   ?= 1
KPM_FUZZ_CORPUS := arrays casts int64 reals records scheduler sets mathfn
KPM_FUZZ_SEEDS := $(KPM_FUZZ_CORPUS:%=build/kpm-fuzz-seeds/%.kpm)

build/kpm-fuzz-seeds/%.kpm: tests/kp-corpus/%.pas build/skj-kp-jit | build
	@mkdir -p build/kpm-fuzz-seeds
	@./build/skj-kp-jit --emit-kpm $< > $@

test-kpm-fuzz: build/san/kpm-fuzz $(KPM_FUZZ_SEEDS)
	@echo "== KPM1 reader fuzz ($(KPM_FUZZ_ITERS) iterations, rng $(KPM_FUZZ_RNG)) =="
	@$(SAN_RUNENV) ./build/san/kpm-fuzz --mutate $(KPM_FUZZ_ITERS) --rng $(KPM_FUZZ_RNG) \
	   tests/kp/kp_reader_ok.kpm tests/kp/kp_reader_call2_nodst.kpm $(KPM_FUZZ_SEEDS)

## The preprocessor fuzz: mutate the cpp test fixtures and preprocess each
## result under ASan+UBSan.  The fixtures already cover object- and
## function-like macros, token paste, stringification, and nested conditionals,
## so the mutations probe those paths.  Same reproducible-PRNG contract as the
## KPM fuzz; the stdin mode (no --mutate) is the AFL++ entry.
CPP_FUZZ_ITERS ?= 50000
CPP_FUZZ_RNG   ?= 1
CPP_FUZZ_SEEDS := $(wildcard tests/cpp_t*.c)
test-cpp-fuzz: build/san/cpp-fuzz | build
	@echo "== preprocessor fuzz ($(CPP_FUZZ_ITERS) iterations, rng $(CPP_FUZZ_RNG)) =="
	@# The preprocessor writes a diagnostic per malformed input, which floods the
	@# console; route it (and any sanitizer report) to a log and surface only the
	@# summary, tailing the log if the run aborts.
	@$(SAN_RUNENV) ./build/san/cpp-fuzz --mutate $(CPP_FUZZ_ITERS) --rng $(CPP_FUZZ_RNG) \
	   $(CPP_FUZZ_SEEDS) >/dev/null 2>build/cpp-fuzz.log \
	   || { echo "cpp-fuzz: FAILED"; tail -40 build/cpp-fuzz.log; exit 1; }
	@tail -1 build/cpp-fuzz.log

## The assembler fuzzers.  Seeds: a generated ColdFire object's asm for skj-as,
## and the committed RV/MIPS encoding fixtures.  Same reproducible-PRNG contract.
AS_FUZZ_ITERS ?= 20000
AS_FUZZ_RNG   ?= 1
## ColdFire seeds: a spread of tinc outputs (recursion, loops, spills, memory,
## continuations) so the mutations reach a broad slice of the encoder.
AS_FUZZ_CF_PROGS := fib loop spill bsearch cont multiret
AS_FUZZ_CF_SEEDS := $(AS_FUZZ_CF_PROGS:%=build/as-fuzz-seeds/%.s)
build/as-fuzz-seeds/%.s: tests/%.tc build/skj-tinc | build
	@mkdir -p build/as-fuzz-seeds
	@./build/skj-tinc -o $@ $<
test-as-fuzz: build/san/as-fuzz $(AS_FUZZ_CF_SEEDS)
	@echo "== ColdFire assembler fuzz ($(AS_FUZZ_ITERS) iterations, rng $(AS_FUZZ_RNG)) =="
	@$(SAN_RUNENV) ./build/san/as-fuzz --mutate $(AS_FUZZ_ITERS) --rng $(AS_FUZZ_RNG) \
	   $(AS_FUZZ_CF_SEEDS) >/dev/null 2>build/as-fuzz.log \
	   || { echo "as-fuzz: FAILED"; tail -40 build/as-fuzz.log; exit 1; }
	@tail -1 build/as-fuzz.log

test-as-rv-fuzz: build/san/as-rv-fuzz
	@echo "== RV32 assembler fuzz ($(AS_FUZZ_ITERS) iterations, rng $(AS_FUZZ_RNG)) =="
	@$(SAN_RUNENV) ./build/san/as-rv-fuzz --mutate $(AS_FUZZ_ITERS) --rng $(AS_FUZZ_RNG) \
	   $(wildcard tests/rvas/*.s) >/dev/null 2>build/as-rv-fuzz.log \
	   || { echo "as-rv-fuzz: FAILED"; tail -40 build/as-rv-fuzz.log; exit 1; }
	@tail -1 build/as-rv-fuzz.log

test-as-mips-fuzz: build/san/as-mips-fuzz
	@echo "== MIPS assembler fuzz ($(AS_FUZZ_ITERS) iterations, rng $(AS_FUZZ_RNG)) =="
	@$(SAN_RUNENV) ./build/san/as-mips-fuzz --mutate $(AS_FUZZ_ITERS) --rng $(AS_FUZZ_RNG) \
	   $(wildcard tests/mipsas/*.s) >/dev/null 2>build/as-mips-fuzz.log \
	   || { echo "as-mips-fuzz: FAILED"; tail -40 build/as-mips-fuzz.log; exit 1; }
	@tail -1 build/as-mips-fuzz.log

test-as-x86-64-fuzz: build/san/as-x86-64-fuzz
	@echo "== x86-64 assembler fuzz ($(AS_FUZZ_ITERS) iterations, rng $(AS_FUZZ_RNG)) =="
	@$(SAN_RUNENV) ./build/san/as-x86-64-fuzz --mutate $(AS_FUZZ_ITERS) --rng $(AS_FUZZ_RNG) \
	   $(wildcard tests/x86-64as/*.s) >/dev/null 2>build/as-x86-64-fuzz.log \
	   || { echo "as-x86-64-fuzz: FAILED"; tail -40 build/as-x86-64-fuzz.log; exit 1; }
	@tail -1 build/as-x86-64-fuzz.log

test-as-aarch64-fuzz: build/san/as-aarch64-fuzz
	@echo "== aarch64 assembler fuzz ($(AS_FUZZ_ITERS) iterations, rng $(AS_FUZZ_RNG)) =="
	@$(SAN_RUNENV) ./build/san/as-aarch64-fuzz --mutate $(AS_FUZZ_ITERS) --rng $(AS_FUZZ_RNG) \
	   $(wildcard tests/aarch64as/*.s) >/dev/null 2>build/as-aarch64-fuzz.log \
	   || { echo "as-aarch64-fuzz: FAILED"; tail -40 build/as-aarch64-fuzz.log; exit 1; }
	@tail -1 build/as-aarch64-fuzz.log

## The IR front-end parser fuzzers.  Seeds are each front end's test sources.
FE_FUZZ_ITERS ?= 20000
FE_FUZZ_RNG   ?= 1
test-fe-tinc-fuzz: build/san/fe-tinc-fuzz
	@echo "== TinC parser fuzz ($(FE_FUZZ_ITERS) iterations, rng $(FE_FUZZ_RNG)) =="
	@$(SAN_RUNENV) ./build/san/fe-tinc-fuzz --mutate $(FE_FUZZ_ITERS) --rng $(FE_FUZZ_RNG) \
	   $(wildcard tests/*.tc) >/dev/null 2>build/fe-tinc-fuzz.log \
	   || { echo "fe-tinc-fuzz: FAILED"; tail -40 build/fe-tinc-fuzz.log; exit 1; }
	@tail -1 build/fe-tinc-fuzz.log

test-fe-pascal-fuzz: build/san/fe-pascal-fuzz
	@echo "== Pascal parser fuzz ($(FE_FUZZ_ITERS) iterations, rng $(FE_FUZZ_RNG)) =="
	@$(SAN_RUNENV) ./build/san/fe-pascal-fuzz --mutate $(FE_FUZZ_ITERS) --rng $(FE_FUZZ_RNG) \
	   $(wildcard tests/pascal_*.pas) >/dev/null 2>build/fe-pascal-fuzz.log \
	   || { echo "fe-pascal-fuzz: FAILED"; tail -40 build/fe-pascal-fuzz.log; exit 1; }
	@tail -1 build/fe-pascal-fuzz.log

test-fe-moo-fuzz: build/san/fe-moo-fuzz
	@echo "== MooScript parser fuzz ($(FE_FUZZ_ITERS) iterations, rng $(FE_FUZZ_RNG)) =="
	@$(SAN_RUNENV) ./build/san/fe-moo-fuzz --mutate $(FE_FUZZ_ITERS) --rng $(FE_FUZZ_RNG) \
	   $(wildcard tests/moo_*.moo) >/dev/null 2>build/fe-moo-fuzz.log \
	   || { echo "fe-moo-fuzz: FAILED"; tail -40 build/fe-moo-fuzz.log; exit 1; }
	@tail -1 build/fe-moo-fuzz.log

test-fe-cc-fuzz: build/san/fe-cc-fuzz
	@echo "== C parser fuzz ($(FE_FUZZ_ITERS) iterations, rng $(FE_FUZZ_RNG)) =="
	@$(SAN_RUNENV) ./build/san/fe-cc-fuzz --mutate $(FE_FUZZ_ITERS) --rng $(FE_FUZZ_RNG) \
	   $(wildcard tests/cc_t*.c) >/dev/null 2>build/fe-cc-fuzz.log \
	   || { echo "fe-cc-fuzz: FAILED"; tail -40 build/fe-cc-fuzz.log; exit 1; }
	@tail -1 build/fe-cc-fuzz.log

## The pure compile-and-emit tools under the sanitizers: the preprocessor, the
## C compiler, every IR front end (tinc, scheme, moo, Pascal, Excelsior), the
## assembler and linker, and the archive tool.  skj-ld is exercised by the
## Excelsior slice (the cc slice links with the system ld).  This is the CI
## slice: self-contained, no RISC-V reference assembler and no cross checkouts.
check-san-tools: check-san-cpp check-san-cc \
                 check-san-tinc check-san-scheme check-san-moo check-san-pc \
                 check-san-ar
	@echo "check-san-tools: preprocessor, compilers, front ends, assembler, linker, and ar clean under the sanitizers."

## The Kobold behavioral corpora through the ASan+UBSan driver.
check-san-kobold: check-san-kp check-san-kp-corpus check-san-kp-multi \
                  check-san-kp-io check-san-kp-inc
	@echo "check-san-kobold: KPM1 reader and the Kobold corpora clean under ASan+UBSan."

## Every sanitized slice.
check-san: check-san-tools check-san-rvas check-san-jit check-san-kobold \
           test-kpm-fuzz test-cpp-fuzz \
           test-as-fuzz test-as-rv-fuzz test-as-mips-fuzz test-as-x86-64-fuzz test-as-aarch64-fuzz \
           test-fe-tinc-fuzz test-fe-pascal-fuzz test-fe-moo-fuzz \
           test-fe-cc-fuzz
	@echo "All check-san suites passed under the sanitizers."

.PHONY: check-san check-san-tools check-san-kobold \
        check-san-cpp check-san-cc check-san-rvas check-san-kp \
        check-san-jit check-san-tinc check-san-scheme check-san-moo \
        check-san-pc check-san-ar check-san-kp-corpus check-san-kp-multi \
        check-san-kp-io check-san-kp-inc test-kpm-fuzz test-cpp-fuzz \
        test-as-fuzz test-as-rv-fuzz test-as-mips-fuzz test-as-x86-64-fuzz test-as-aarch64-fuzz \
        test-fe-tinc-fuzz test-fe-pascal-fuzz test-fe-moo-fuzz \
        test-fe-cc-fuzz

## arm64 per-test rules: tests/<name>.tc -> build/arm64/<name>.s -> .o -> binary
A64_TESTS := $(TESTS)
A64_SCM_TESTS := $(SCM_TESTS)

build/arm64/%.s: tests/%.tc build/skj-tinc-arm64 | build/arm64
	./build/skj-tinc-arm64 -o $@ $<

build/arm64/scm_%.s: tests/scm_%.scm build/skj-sc-arm64 | build/arm64
	./build/skj-sc-arm64 -o $@ $<

build/arm64/%.o: build/arm64/%.s $(A64_ASM_DEP)
	$(A64_AS) -o $@ $<

build/arm64/start.o: runtime/start_arm64.S $(A64_ASM_DEP) | build/arm64
	$(A64_AS) -o $@ $<

build/arm64/start_aapcs.o: runtime/start_arm64_aapcs.S $(A64_ASM_DEP) | build/arm64
	$(A64_AS) -o $@ $<

build/arm64/va.o: runtime/va_aarch64.c | build/arm64
	$(A64_CC) -std=c99 -O2 -Wall -ffreestanding -c -o $@ $<

build/arm64/soft64.o: runtime/soft64.c | build/arm64
	$(A64_CC) -std=c99 -O2 -Wall -ffreestanding -DSOFT64_NO_MULSHIFT -c -o $@ $<

build/arm64/%: build/arm64/%.o build/arm64/start.o
	$(A64_LD) -o $@ build/arm64/start.o $<

build/arm64:
	mkdir -p build/arm64

check-arm64: build/skj-tinc-arm64 build/skj-sc-arm64 $(A64_TESTS:tests/%.tc=build/arm64/%) $(A64_SCM_TESTS:tests/%.scm=build/arm64/%)
	@sh tests/run-tests.sh "$(QEMU_A64)" build/arm64

## x86-64 per-test rules: tests/<name>.tc -> build/x86_64/<name>.s -> .o -> binary
X64_TESTS := $(TESTS)
X64_SCM_TESTS := $(SCM_TESTS)

build/x86_64/%.s: tests/%.tc build/skj-tinc-x86-64 | build/x86_64
	./build/skj-tinc-x86-64 -o $@ $<

build/x86_64/scm_%.s: tests/scm_%.scm build/skj-sc-x86-64 | build/x86_64
	./build/skj-sc-x86-64 -o $@ $<

build/x86_64/%.o: build/x86_64/%.s $(X64_ASM_DEP)
	$(X64_ASM) $(X64_ASMFLAGS) -o $@ $<

build/x86_64/start.o: runtime/start_x86_64.s $(X64_ASM_DEP) | build/x86_64
	$(X64_ASM) $(X64_ASMFLAGS) -o $@ $<

build/x86_64/start_sysv.o: runtime/start_x86_64_sysv.s $(X64_ASM_DEP) | build/x86_64
	$(X64_ASM) $(X64_ASMFLAGS) -o $@ $<

build/x86_64/va.o: runtime/va_x86_64.c | build/x86_64
	$(CC) -std=c99 -O2 -Wall -ffreestanding -c -o $@ $<

build/x86_64/%: build/x86_64/%.o build/x86_64/start.o
	$(X64_LD) -m elf_x86_64 -o $@ build/x86_64/start.o $<

build/x86_64:
	mkdir -p build/x86_64

check-x86-64: build/skj-tinc-x86-64 build/skj-sc-x86-64 $(X64_TESTS:tests/%.tc=build/x86_64/%) $(X64_SCM_TESTS:tests/%.scm=build/x86_64/%)
	@sh tests/run-tests.sh "$(QEMU_X64)" build/x86_64

## TinScheme compiler
SC_SRC := scheme/lex.c scheme/parse.c scheme/print.c scheme/main.c scheme/gc.c \
          scheme/lower.c $(IR_SRC) $(BE_CF)
SC_SRC_RV := scheme/lex.c scheme/parse.c scheme/print.c scheme/main.c scheme/gc.c \
             scheme/lower.c $(IR_SRC) $(BE_RV)
SC_SRC_MIPS := scheme/lex.c scheme/parse.c scheme/print.c scheme/main.c scheme/gc.c \
               scheme/lower.c $(IR_SRC) $(BE_MIPS)
SC_SRC_A64 := scheme/lex.c scheme/parse.c scheme/print.c scheme/main.c scheme/gc.c \
              scheme/lower.c $(IR_SRC) $(BE_A64)

build/skj-sc: $(SC_SRC) scheme/scheme.h scheme/gc.h ir/ir.h | build
	$(CC) $(CFLAGS) -Ischeme -Iir -o $@ $(SC_SRC)

build/skj-sc-rv: $(SC_SRC_RV) scheme/scheme.h scheme/gc.h ir/ir.h | build
	$(CC) $(CFLAGS) -Ischeme -Iir -o $@ $(SC_SRC_RV)

build/skj-sc-mips: $(SC_SRC_MIPS) scheme/scheme.h scheme/gc.h ir/ir.h | build
	$(CC) $(CFLAGS) -Ischeme -Iir -o $@ $(SC_SRC_MIPS)

build/skj-sc-arm64: $(SC_SRC_A64) scheme/scheme.h scheme/gc.h ir/ir.h | build
	$(CC) $(CFLAGS) -DSKJ_LP64=1 -Ischeme -Iir -o $@ $(SC_SRC_A64)

SC_SRC_X64 := scheme/lex.c scheme/parse.c scheme/print.c scheme/main.c scheme/gc.c \
              scheme/lower.c $(IR_SRC) $(BE_X64)

build/skj-sc-x86-64: $(SC_SRC_X64) scheme/scheme.h scheme/gc.h ir/ir.h | build
	$(CC) $(CFLAGS) -DX86_BITS=64 -DSKJ_LP64=1 -Ischeme -Iir -o $@ $(SC_SRC_X64)

## TinScheme codegen tests (.scm -> .s via skj-sc)
build/scm_%.s: tests/scm_%.scm build/skj-sc | build
	./build/skj-sc -o $@ $<

## TinScheme RISC-V codegen tests
build/rv/scm_%.s: tests/scm_%.scm build/skj-sc-rv | build/rv
	./build/skj-sc-rv -o $@ $<

## TinScheme GC test (host-only)
build/test_gc: scheme/test_gc.c scheme/gc.c scheme/gc.h | build
	$(CC) $(CFLAGS) -Ischeme -o $@ scheme/test_gc.c scheme/gc.c

## RNG generators unit test (host-only): SplitMix64, xoroshiro128** and
## ChaCha20 against known-answer vectors.  See doc/tools and boris A4.
test-rng: build/rng_test
	./build/rng_test

build/rng_test: tests/rng_test.c runtime/rng.c runtime/rng.h | build
	$(CC) $(CFLAGS) -Iruntime -o $@ tests/rng_test.c runtime/rng.c

## RNG/clock glue unit test (host-only): the guest-facing kp_random_* / kp_now_ms
## entry points, with the getrandom/clock_gettime seam stubbed deterministically.
test-rng-glue: build/rng_glue_test
	./build/rng_glue_test

build/rng_glue_test: tests/rng_glue_test.c runtime/rng_glue.c runtime/rng.c runtime/rng.h | build
	$(CC) $(CFLAGS) -Iruntime -o $@ tests/rng_glue_test.c runtime/rng_glue.c runtime/rng.c

## Sleep glue unit test (host-only): the guest-facing kp_sleep_ms entry point,
## with the nanosleep seam stubbed so nothing sleeps.
test-sched-glue: build/sched_glue_test
	./build/sched_glue_test

build/sched_glue_test: tests/sched_glue_test.c runtime/sched_glue.c | build
	$(CC) $(CFLAGS) -Iruntime -o $@ tests/sched_glue_test.c runtime/sched_glue.c

test-gc: build/test_gc
	./build/test_gc

## The libc coroutine primitive (host-only quick gate).  Compiles libc/coro.c
## with the x86-64 co_switch natively and runs the generator/echo self-check, so
## the primitive has a no-qemu smoke test; the per-target qemu runs through
## check-libc cover rv/mips/cf/arm64.  x86-64 host only.
build/test_coro: libc/coro.c runtime/co_switch_x86.S tests/libc_coroutine.c \
                 include/coro.h | build
	$(CC) $(CFLAGS) -Iinclude -o $@ libc/coro.c runtime/co_switch_x86.S \
	    tests/libc_coroutine.c

test-coro: build/test_coro
	@./build/test_coro; ec=$$?; \
	 if [ $$ec -eq 42 ]; then echo "PASS: libc coroutine (host, exit 42)"; \
	 else echo "FAIL: libc coroutine (exit $$ec, expected 42)"; exit 1; fi

## The cooperative threads.h subset (host-only quick gate).  Compiles the
## scheduler (libc/threads.c) over the coroutine primitive with the x86-64
## co_switch natively and runs the producer/consumer + mutex/cnd/once/tss
## self-check, so threads.h has a no-qemu smoke test; the per-target qemu runs
## through check-libc cover rv/mips/cf/arm64.  x86-64 host only.
build/test_threads: libc/threads.c libc/coro.c runtime/co_switch_x86.S \
                    tests/libc_threads_stress.c include/threads.h include/coro.h | build
	$(CC) $(CFLAGS) -Iinclude -o $@ libc/threads.c libc/coro.c \
	    runtime/co_switch_x86.S tests/libc_threads_stress.c

test-threads: build/test_threads
	@./build/test_threads; ec=$$?; \
	 if [ $$ec -eq 42 ]; then echo "PASS: libc threads (host, exit 42)"; \
	 else echo "FAIL: libc threads (exit $$ec, expected 42)"; exit 1; fi

## RV32 interrupt delivery (host-only: it drives the core directly, so
## it needs neither a cross toolchain nor qemu)
build/test_rv_irq: tests/test_rv_irq.c emu/rv32.c emu/rv32.h | build
	$(CC) $(CFLAGS) -Iemu -o $@ tests/test_rv_irq.c emu/rv32.c -lm

test-rv-irq: build/test_rv_irq
	@./build/test_rv_irq

## The rv_run() batch contract: the reason it returns (BUDGET/HALT/TRAP/
## YIELD), the retired count, and that it stops early on a trap, a wfi
## park or a halt rather than spending the whole budget.  Host-only.
build/test_rv_run: tests/test_rv_run.c emu/rv32.c emu/rv32.h | build
	$(CC) $(CFLAGS) -Iemu -o $@ tests/test_rv_run.c emu/rv32.c -lm

test-rv-run: build/test_rv_run
	@./build/test_rv_run

## The guest dynamic-mapping primitives: the guard page (a stack overrun
## must fault), gm_munmap release, the gm_pool aggregate budget, and
## gm_read_out.  Drives emu/guest.c directly, no CPU core.  Host-only.
build/test_guest_mmap: tests/test_guest_mmap.c emu/guest.c emu/guest.h \
                       emu/hostplat.c emu/hostplat.h emu/elf32.c emu/elf32.h | build
	$(CC) $(CFLAGS) -Iemu -Ifs -o $@ tests/test_guest_mmap.c \
	    emu/guest.c emu/hostplat.c emu/elf32.c -lm

test-guest-mmap: build/test_guest_mmap
	@./build/test_guest_mmap

## The decoded-instruction cache: run the same program with the cache on and
## off in lockstep and confirm the architectural state matches after every
## instruction.  The cache is a hint, so its whole contract is that it changes
## no result.  Host-only.
build/test_rv_decode_cache: tests/test_rv_decode_cache.c emu/rv32.c emu/rv32.h | build
	$(CC) $(CFLAGS) -Iemu -o $@ tests/test_rv_decode_cache.c emu/rv32.c -lm

test-rv-decode-cache: build/test_rv_decode_cache
	@./build/test_rv_decode_cache

## Compressed-expansion offsets, checked against what an assembler
## produces rather than against the expander itself.  Also host-only.
build/test_rv_expand: tests/test_rv_expand.c emu/rv32.c emu/rv32.h | build
	$(CC) $(CFLAGS) -Iemu -o $@ tests/test_rv_expand.c emu/rv32.c -lm

test-rv-expand: build/test_rv_expand
	@./build/test_rv_expand

## How an access reaches the bus, which is what an embedder's devices
## see and what no value-checking test can distinguish.
build/test_rv_bus: tests/test_rv_bus.c emu/rv32.c emu/rv32.h | build
	$(CC) $(CFLAGS) -Iemu -o $@ tests/test_rv_bus.c emu/rv32.c -lm

test-rv-bus: build/test_rv_bus
	@./build/test_rv_bus

## The CSR zero-source rules, and the fcsr windows.  Also the only
## thing in the tree that runs csrrc, csrrci or csrrsi at all.
build/test_rv_csr: tests/test_rv_csr.c emu/rv32.c emu/rv32.h | build
	$(CC) $(CFLAGS) -Iemu -o $@ tests/test_rv_csr.c emu/rv32.c -lm

test-rv-csr: build/test_rv_csr
	@./build/test_rv_csr

## Frame sizes, and the encodings that must not decode at all.
build/test_rv_decode: tests/test_rv_decode.c emu/rv32.c emu/rv32.h | build
	$(CC) $(CFLAGS) -Iemu -o $@ tests/test_rv_decode.c emu/rv32.c -lm

test-rv-decode: build/test_rv_decode
	@./build/test_rv_decode

## The sign of a zero sum, which is the one addition result whose sign
## the value does not imply.
build/test_rv_fp: tests/test_rv_fp.c emu/rv32.c emu/rv32.h | build
	$(CC) $(CFLAGS) -Iemu -o $@ tests/test_rv_fp.c emu/rv32.c -lm

test-rv-fp: build/test_rv_fp
	@./build/test_rv_fp

## ---------------------------------------------------------------
## The RV32 test rig (tests/rv32)
##
## Six verification methods, from the five-article series this core came
## out of.  Every one runs alone, which is what let the last article
## measure what each is worth:
##
##   unit      seven hand-written suites, 443 checks, no reference model
##   program   one compiled guest that validates its own results
##   apps      CoreMark and Lua, which validate their own results
##   archtest  268 compliance tests, signatures compared against qemu
##   fuzz      randomized encodings, compared against qemu per instruction
##   lockstep  compiled guests, compared against qemu per instruction
##
## The audit found that coverage and mutation disagree about which to
## keep: lockstep contributes no unique line and no unique instruction,
## and kills four mutants nothing else kills.  Coverage measures reach,
## mutation measures whether a wrong answer would be noticed.  Neither
## alone is a reason to drop a method.
##
## The files are byte-for-byte upstream so a re-sync stays a copy; the
## one addition is tests/rv32/elf_loader.h, which maps their loader
## interface onto emu/elf32.c rather than carrying a second ELF reader.
## ---------------------------------------------------------------
RV32_UNIT := test_mem test_fp test_fp_d test_atomic test_zcmp test_zcb \
             test_bitmanip test_irq
RV32_INC  := -Iemu -Ifs -Itests/rv32
RV32_CORE := emu/rv32.c tests/rv32/machine.c
RV32_LOAD := emu/elf32.c emu/guest.c emu/hostplat.c

## gdbclient speaks the GDB remote protocol over a socket, and the POSIX
## headers for that are hidden by -std=c99.
RV32_SOCK_CFLAGS := $(subst -std=c99,-std=gnu99,$(CFLAGS))

build/rv32:
	mkdir -p build/rv32

## The unit suites need the core and the flat test machine, nothing else.
$(RV32_UNIT:%=build/rv32/%): build/rv32/%: tests/rv32/%.c $(RV32_CORE) \
		emu/rv32.h tests/rv32/machine.h | build/rv32
	$(CC) $(CFLAGS) $(RV32_INC) -o $@ $< $(RV32_CORE) -lm

test-rv32-unit: $(RV32_UNIT:%=build/rv32/%)
	@fail=0; \
	for t in $(RV32_UNIT); do \
	    out=$$(./build/rv32/$$t 2>&1 | tail -1); \
	    case "$$out" in \
	    *"0 failures"*) printf 'PASS  %-14s %s\n' "$$t" "$$out" ;; \
	    *) printf 'FAIL  %-14s %s\n' "$$t" "$$out"; fail=1 ;; \
	    esac; \
	done; \
	[ $$fail -eq 0 ]

## The reference-model harnesses.  gdbclient drives qemu-riscv32 over the
## GDB remote protocol, which is how an instruction-by-instruction
## comparison is taken.
build/rv32/lockstep: tests/rv32/lockstep.c tests/rv32/gdbclient.c $(RV32_CORE) \
		$(RV32_LOAD) tests/rv32/elf_loader.h | build/rv32
	$(CC) $(RV32_SOCK_CFLAGS) $(RV32_INC) -o $@ \
		tests/rv32/lockstep.c tests/rv32/gdbclient.c $(RV32_CORE) \
		$(RV32_LOAD) -lm

build/rv32/fuzz: tests/rv32/fuzz.c tests/rv32/gdbclient.c $(RV32_CORE) \
		$(RV32_LOAD) tests/rv32/elf_loader.h | build/rv32
	$(CC) $(RV32_SOCK_CFLAGS) $(RV32_INC) -o $@ \
		tests/rv32/fuzz.c tests/rv32/gdbclient.c $(RV32_CORE) \
		$(RV32_LOAD) -lm

build/rv32/test_harness: tests/rv32/test_harness.c $(RV32_CORE) $(RV32_LOAD) \
		tests/rv32/elf_loader.h | build/rv32
	$(CC) $(CFLAGS) $(RV32_INC) -o $@ tests/rv32/test_harness.c \
		$(RV32_CORE) $(RV32_LOAD) -lm

## Guests for the reference-model methods.  RV32_GUEST_ARCH names the
## whole ISA this core implements, so a compiler is free to emit any of
## it and the comparison covers what a real program would use.
RV32_GUEST_ARCH := rv32imafc_zicsr_zifencei_zba_zbb_zbs_zcb
RV32_GUEST_CFLAGS := -march=$(RV32_GUEST_ARCH) -mabi=ilp32f -O2 -nostdlib \
                     -static -ffreestanding -fno-pic -no-pie \
                     -Wl,--build-id=none

build/rv32/%.elf: tests/rv32/%.c tests/rv32/start.S tests/rv32/link.ld | build/rv32
	$(RV_CC) $(RV32_GUEST_CFLAGS) -T tests/rv32/link.ld -o $@ \
		tests/rv32/start.S $<

build/rv32/fuzz_target.elf: tests/rv32/fuzz_target.S tests/rv32/link.ld | build/rv32
	$(RV_CC) $(RV32_GUEST_CFLAGS) -T tests/rv32/link.ld -o $@ $<

## A guest built for whole-frame push and pop.  GNU as cannot assemble
## Zcmp, so clang produces the objects and GNU ld places them; without
## clang this guest is simply absent and lockstep runs the others.
CLANG   ?= clang
RV_LDBIN ?= riscv64-linux-gnu-ld

build/rv32/test_program_zcmp.elf: tests/rv32/test_program.c tests/rv32/start.S \
		tests/rv32/link.ld | build/rv32
	@command -v $(CLANG) >/dev/null || { \
	    echo "skip $@: no $(CLANG) (GNU as cannot assemble Zcmp)"; exit 0; }; \
	$(CLANG) --target=riscv32 -march=rv32imfc_zcmp -mabi=ilp32f -O2 \
	    -ffreestanding -fno-pic -mno-relax -c -o build/rv32/tp_zcmp.o \
	    tests/rv32/test_program.c && \
	$(CLANG) --target=riscv32 -march=rv32imfc_zcmp -mabi=ilp32f \
	    -fno-pic -mno-relax -c -o build/rv32/start_zcmp.o tests/rv32/start.S && \
	$(RV_LDBIN) -m elf32lriscv -T tests/rv32/link.ld --build-id=none -o $@ \
	    build/rv32/start_zcmp.o build/rv32/tp_zcmp.o

## The guests lockstep needs to reach the extensions a plain C program
## never makes the compiler emit.
RV32_LOCKSTEP_GUESTS := build/rv32/test_program.elf \
                        build/rv32/bitmanip_guest.elf \
                        build/rv32/zcb_guest.elf \
                        build/rv32/test_program_zcmp.elf

RV32_CPU_ZB   := rv32,zba=true,zbb=true,zbs=true,zcb=true
RV32_CPU_ZCMP := rv32,c=false,zca=true,zcf=true,zcmp=true

## The `program` method: one guest that checks its own results.
test-rv32-program: build/rv32/test_harness build/rv32/test_program.elf
	@./build/rv32/test_harness build/rv32/test_program.elf

## `lockstep` and `fuzz` need qemu-riscv32 as the reference.  Each guest
## is run against the CPU model that matches how it was built.
test-rv32-lockstep: build/rv32/lockstep $(RV32_LOCKSTEP_GUESTS)
	@./build/rv32/lockstep build/rv32/test_program.elf
	@./build/rv32/lockstep build/rv32/bitmanip_guest.elf -cpu $(RV32_CPU_ZB)
	@./build/rv32/lockstep build/rv32/zcb_guest.elf -cpu $(RV32_CPU_ZB)
	@if [ -s build/rv32/test_program_zcmp.elf ]; then \
	    ./build/rv32/lockstep build/rv32/test_program_zcmp.elf \
	        -cpu $(RV32_CPU_ZCMP); \
	fi

## The generator is deterministic: one fixed seed, -s to change it. So a
## run is not a sample, it is a fixed sequence, and the only thing that
## buys more ground is a longer one. The budget is therefore the whole
## question, and 50 rounds was too short to be load-bearing.
##
## Measured against a mutant that gets the sign of a zero sum wrong,
## which needs an fadd.s whose operands are both exactly -0, about one
## in 1600 of them: it survived 120 rounds and died at 600. 600 rounds
## is 307,200 instructions and about three minutes, in the same range
## as the compliance suite, and this target is opt-in.
##
## Raising it is one number; a different seed is RV32_FUZZ_ARGS="-s 2",
## which explores ground this sequence never reaches.
RV32_FUZZ_N ?= 600
RV32_FUZZ_ARGS ?=

test-rv32-fuzz: build/rv32/fuzz build/rv32/fuzz_target.elf
	@./build/rv32/fuzz build/rv32/fuzz_target.elf -n $(RV32_FUZZ_N) \
		$(RV32_FUZZ_ARGS)

## riscv-arch-test.  A bare-metal board (RAM at 0x80000000, a 16550
## transmitter, a CLINT and a test finisher) rather than the Linux user
## mode skj-run provides, since the compliance binaries are machine-mode
## images.  The suite ships no reference signatures, so each test dumps
## its own over the serial port and the same binary runs on
## qemu-system-riscv32 for the comparison.
build/rv32/archtest: tests/rv32/archtest.c emu/rv32.c $(RV32_LOAD) \
		tests/rv32/elf_loader.h | build/rv32
	$(CC) $(RV32_SOCK_CFLAGS) $(RV32_INC) -o $@ \
		tests/rv32/archtest.c emu/rv32.c $(RV32_LOAD) -lm

## The suite is external and large, so it is not vendored: point
## ARCHTEST at the riscv-test-suite directory of a checkout of
## riscv-arch-test (branch old-framework-3.x).  Without it the target
## says what it needs and stops, rather than reporting a pass it did not
## run.
ARCHTEST ?=

check-archtest: build/rv32/archtest
	@if [ -z "$(ARCHTEST)" ]; then \
	    echo "check-archtest: set ARCHTEST=<path>/riscv-test-suite"; \
	    echo "  a checkout of riscv-arch-test, branch old-framework-3.x"; \
	    exit 2; \
	fi
	@sh tests/rv32/run-archtest.sh "$(ARCHTEST)" $(ARCHTEST_SUITES)

## ---------------------------------------------------------------
## The `apps` method: real programs that check their own answers
##
## Neither is vendored, so each needs a checkout and (for Lua) a second
## toolchain.  What they buy over the hand-written guests is that nobody
## wrote them with this emulator in mind: CoreMark validates its own
## workload CRCs, and Lua allocates constantly, uses setjmp and longjmp,
## formats floating point, and runs a script that checks its own results.
## If Lua runs, most of a C library and most of the instruction set are
## working together.
## ---------------------------------------------------------------
CM ?=
CM_ITERATIONS ?= 100

## CoreMark's barebones target wants four things from a board: a clock,
## board init, a character sink and the seed variables; coremark-port
## supplies them.  Its own ee_printf is reused with the placeholder
## character sink cut out, since the port has the real one.
build/rv32/coremark.elf: tests/rv32/coremark-port/core_portme.c \
		tests/rv32/coremark-port/core_portme.h tests/rv32/start.S \
		tests/rv32/link.ld | build/rv32
	@if [ -z "$(CM)" ] || [ ! -d "$(CM)" ]; then \
	    echo "coremark.elf: set CM to a coremark checkout"; \
	    echo "  git clone --depth 1 https://github.com/eembc/coremark.git"; \
	    exit 1; \
	fi
	@perl -0pe 's{void\nuart_send_char\(char c\)\n\{.*?\n\}\n}{/* uart_send_char is supplied by the port */\n}s' \
	    $(CM)/barebones/ee_printf.c > build/rv32/ee_printf_port.c
	$(RV_CC) $(RV32_GUEST_CFLAGS) -T tests/rv32/link.ld \
	    -I tests/rv32/coremark-port -I $(CM) \
	    -DPERFORMANCE_RUN=1 -DITERATIONS=$(CM_ITERATIONS) -DHAS_FLOAT=0 \
	    -DHAS_TIME_H=0 -DUSE_CLOCK=0 -DHAS_PRINTF=0 \
	    -DFLAGS_STR='"$(RV32_GUEST_CFLAGS)"' -o $@ \
	    tests/rv32/start.S $(CM)/core_list_join.c $(CM)/core_main.c \
	    $(CM)/core_matrix.c $(CM)/core_state.c $(CM)/core_util.c \
	    tests/rv32/coremark-port/core_portme.c build/rv32/ee_printf_port.c

## Lua needs a C library, which the stock riscv cross toolchain has none
## of.  picolibc supplies one:
##   apt-get install gcc-riscv64-unknown-elf picolibc-riscv64-unknown-elf
##   git clone --depth 1 -b v5.4.7 https://github.com/lua/lua.git
LUA     ?=
LUA_CC  ?= riscv64-unknown-elf-gcc
LUA_ARCH ?= rv32imafc_zba_zbb_zbs
LUA_FLAGS = -march=$(LUA_ARCH) -mabi=ilp32f -O2 --specs=picolibc.specs \
            -DLUA_32BITS=0 -DLUA_USE_C89=0 \
            -Wl,--build-id=none -T tests/rv32/lua-port/lua.ld -nostartfiles

## Everything but lua.c and onelua.c, which carry their own main.
LUA_SRCS = $(filter-out $(LUA)/lua.c $(LUA)/onelua.c,$(wildcard $(LUA)/*.c))

build/rv32/lua.elf: tests/rv32/lua-port/lua_port.c \
		tests/rv32/lua-port/lua_start.S tests/rv32/lua-port/lua.ld | build/rv32
	@if [ -z "$(LUA)" ] || [ ! -d "$(LUA)" ]; then \
	    echo "lua.elf: set LUA to a lua checkout (v5.4.7)"; \
	    echo "  git clone --depth 1 -b v5.4.7 https://github.com/lua/lua.git"; \
	    exit 1; \
	fi
	@command -v $(LUA_CC) >/dev/null || { \
	    echo "lua.elf: no $(LUA_CC); it needs picolibc-riscv64-unknown-elf"; \
	    exit 1; }
	$(LUA_CC) $(LUA_FLAGS) -I $(LUA) -o $@ \
	    tests/rv32/lua-port/lua_start.S tests/rv32/lua-port/lua_port.c \
	    $(LUA_SRCS) -lm

test-rv32-apps: build/rv32/test_harness build/rv32/coremark.elf build/rv32/lua.elf
	@./build/rv32/test_harness build/rv32/coremark.elf
	@./build/rv32/test_harness build/rv32/lua.elf

## Everything in the rig that needs no reference model and no download.
check-rv32: test-rv32-unit test-rv32-program
	@echo "rv32 rig: unit suites and the self-checking guest passed."

## ---------------------------------------------------------------
## Auditing the rig
##
## The three measurements from the last article in the series.  They ask
## what the tests are worth rather than whether they pass, they are slow,
## and they answer differently: coverage says what was never tried,
## mutation says what was tried without being checked.  Neither is a
## build gate; both are read.
##
## Each takes ARCHTEST the way check-archtest does, and runs the methods
## it can find without it.
## ---------------------------------------------------------------
RV32_AUDIT_GUESTS := $(RV32_LOCKSTEP_GUESTS) build/rv32/fuzz_target.elf

## Which lines of the core each method reaches, and which it alone
## reaches.  An aggregate percentage answers neither question.
audit-rv32-coverage: $(RV32_AUDIT_GUESTS)
	@ACT="$(ARCHTEST)" sh tests/rv32/coverage-by-method.sh $(AUDIT_METHODS)

## The same question in the unit that suits an emulator: which guest
## instructions ran, not which lines of C.  One switch arm serves eight
## instructions, so line coverage flatters a decoder.
audit-rv32-icov: $(RV32_AUDIT_GUESTS)
	@ACT="$(ARCHTEST)" sh tests/rv32/icov-by-method.sh $(AUDIT_METHODS)

## Whether the rig would notice if the core were wrong.  MUTANTS is how
## many to try, MUTANT_RIG is how much of the rig to judge them with
## (unit, full, all).
## MUTANT_FUZZ_N is how long the fuzzer runs against each mutant that
## the unit stage failed to kill.  It trades audit time for judgement:
## the rare operand pairs need hundreds of rounds, so a score from this
## rig is a lower bound on what test-rv32-fuzz (600 rounds) would catch.
MUTANTS ?= 40
MUTANT_RIG ?= unit
MUTANT_FUZZ_N ?= 60

audit-rv32-mutants: $(RV32_AUDIT_GUESTS)
	@ACT="$(ARCHTEST)" FUZZ_N=$(MUTANT_FUZZ_N) sh tests/rv32/mutate.sh \
		-n $(MUTANTS) -m $(MUTANT_RIG)

## MooScript compiler
MOO_SRC := moo/lex.c moo/parse.c moo/typecheck.c moo/lower.c moo/main.c $(IR_SRC) $(BE_CF)

build/skj-mooc: $(MOO_SRC) moo/moo.h ir/ir.h | build
	$(CC) $(CFLAGS) -Imoo -Iir -o $@ $(MOO_SRC)

## Excelsior compiler (reader + resolver + type checker + IR lowering,
## ColdFire backend). Lowering covers the integer / bool core so far.
## MooScript runtime objects (cross-compiled C)
build/str.o: runtime/str.c | build
	$(M68K_CC) -std=c99 -O2 -Wall -ffreestanding -c -o $@ $<

build/list.o: runtime/list.c | build
	$(M68K_CC) -std=c99 -O2 -Wall -ffreestanding -c -o $@ $<

build/host_stub.o: runtime/host_stub.c | build
	$(M68K_CC) -std=c99 -O2 -Wall -ffreestanding -c -o $@ $<

build/toy_host.o: runtime/toy_host.c | build
	$(M68K_CC) -std=c99 -O2 -Wall -ffreestanding -c -o $@ $<

MOO_RT_OBJS     := build/start.o build/str.o build/list.o build/host_stub.o
MOO_TOY_RT_OBJS := build/start.o build/str.o build/list.o build/toy_host.o

## MooScript codegen tests (.moo -> .s via skj-mooc)
build/moo_%.s: tests/moo_%.moo build/skj-mooc | build
	./build/skj-mooc -o $@ $<

## MooScript test binaries need runtime objects
MOO_BINS := $(MOO_TESTS:tests/%.moo=build/%)
$(MOO_BINS): build/%: build/%.o $(MOO_RT_OBJS) build/skj-ld
	./build/skj-ld -o $@ $(MOO_RT_OBJS) $<

## MooScript toy test binaries (use toy_host runtime)
MOO_TOY_BINS := $(MOO_TOY_TESTS:tests/%.moo=build/%)
$(MOO_TOY_BINS): build/%: build/%.o $(MOO_TOY_RT_OBJS) build/skj-ld
	./build/skj-ld -o $@ $(MOO_TOY_RT_OBJS) $<

## Compact Pascal compiler
PC_SRC := pascal/lex.c pascal/parse.c pascal/lower.c pascal/main.c $(IR_SRC) $(BE_CF)

build/skj-pc: $(PC_SRC) pascal/pascal.h ir/ir.h | build
	$(CC) $(CFLAGS) -Ipascal -Iir -o $@ $(PC_SRC)

## ---- Native host Pascal compilers (x86-64, aarch64) ----
## The front end is backend-agnostic; each host target is a separate binary
## linked with that target's shared selector, mirroring skj-cc.  PC_LP64 tells
## the front end the target is LP64, so it emits the 64-bit address opcodes
## (IR_LEA64, IR_LDL64, IR_LD64, ...) the IR reserves for a 64-bit pointer.
PC_FE      := pascal/lex.c pascal/parse.c pascal/lower.c pascal/main.c
PC_SRC_X64 := $(PC_FE) $(IR_SRC) $(BE_X64_SEL)
PC_SRC_A64 := $(PC_FE) $(IR_SRC) $(BE_A64_SEL)

build/skj-pc-x86-64: $(PC_SRC_X64) pascal/pascal.h ir/ir.h backend/mc_text.h backend/x86_select.h | build
	$(CC) $(CFLAGS) -DX86_BITS=64 -DSKJ_LP64=1 -Ipascal -Iir -Ibackend -o $@ $(PC_SRC_X64)

build/skj-pc-arm64: $(PC_SRC_A64) pascal/pascal.h ir/ir.h backend/arm64_mc.h backend/arm64_select.h backend/arm64_mc_text.h | build
	$(CC) $(CFLAGS) -DSKJ_LP64=1 -Ipascal -Iir -Ibackend -o $@ $(PC_SRC_A64)

## Host Pascal runtime (native libc): x86-64 built by the host cc, aarch64 by
## the cross cc for running under qemu.
build/pascal_rt_host.o: runtime/pascal_rt.c | build
	$(CC) -std=c99 -O2 -DPAS_HOST -c -o $@ $<
build/pascal_rt_host_a64.o: runtime/pascal_rt.c | build
	$(A64_CC) -std=c99 -O2 -DPAS_HOST -c -o $@ $<
## ASan build of the host runtime, for the leak gate below.  Built with the
## sanitizer so malloc/free in the runtime are the instrumented allocator and
## LeakSanitizer runs at exit over the generated program's heap strings.
build/pascal_rt_host_asan.o: runtime/pascal_rt.c | build
	$(CC) -std=c99 -O1 -g -fsanitize=address -DPAS_HOST -c -o $@ $<

## Run the existing Pascal suite natively on each host backend.  The x86-64
## host check also runs the leak gate (check-pc-host-leak) so a leak, double
## free or use after free in the managed string runtime fails the suite.
check-pc-host: check-pc-host-leak build/skj-pc-x86-64 build/pascal_rt_host.o
	@MODE=x64 PC=build/skj-pc-x86-64 RT=build/pascal_rt_host.o \
	    NASM="$(X64_ASM)" ASMFLAGS="$(X64_ASMFLAGS)" CC="$(CC)" sh tests/run-pc-host.sh $(PAS_TESTS)

check-pc-arm64: build/skj-pc-arm64 build/pascal_rt_host_a64.o
	@MODE=a64 PC=build/skj-pc-arm64 RT=build/pascal_rt_host_a64.o \
	    A64CC="$(A64_CC)" QEMU="$(QEMU_A64)" sh tests/run-pc-host.sh $(PAS_TESTS)

## Leak gate: the x86-64 host suite run under AddressSanitizer + LeakSanitizer,
## so each test also fails on a leak, double free or use after free of the
## heap managed string/array runtime.  Native x86-64 only (LSan is unreliable
## under qemu-user, so the aarch64 suite stays value-only).
check-pc-host-leak: build/skj-pc-x86-64 build/pascal_rt_host_asan.o
	@MODE=x64 SAN=1 PC=build/skj-pc-x86-64 RT=build/pascal_rt_host_asan.o \
	    NASM="$(X64_ASM)" ASMFLAGS="$(X64_ASMFLAGS)" CC="$(CC)" sh tests/run-pc-host.sh $(PAS_TESTS)
.PHONY: check-pc-host-leak

## kpcom acceptance: compile the real Kobold compiler (kobold/kpcom.pas) with
## the skj-pc front end and check it emits byte-identical KPM to the reference
## front end (build/skj-kp-jit) over the whole behavioral corpus.  Prints the
## kpcom build wall time (the long pole) as a metric line.  x86-64 host only.
check-pc-kpcom: build/skj-pc-x86-64 build/pascal_rt_host.o build/skj-kp-jit
	@PC=build/skj-pc-x86-64 RT=build/pascal_rt_host.o \
	    HOST=tests/pascal_kpcom_host.c SRC=$(KPCOM_SRC_PAS) \
	    REF=build/skj-kp-jit CORPUS=tests/kp-corpus OUT=build \
	    NASM="$(X64_ASM)" ASMFLAGS="$(X64_ASMFLAGS)" CC="$(CC)" sh tests/run-pc-kpcom.sh
.PHONY: check-pc-kpcom

## Pascal runtime (cross-compiled C)
build/pascal_rt.o: runtime/pascal_rt.c | build
	$(M68K_CC) -std=c99 -O2 -Wall -ffreestanding -c -o $@ $<

## Vendored UTF-8 decoder (freestanding), for R7 code-point string ops
build/utf8.o: runtime/utf8.c runtime/utf8.h | build
	$(M68K_CC) -std=c99 -O2 -Wall -ffreestanding -c -o $@ $<

## Pascal codegen tests (.pas -> .s via skj-pc)
build/pascal_%.s: tests/pascal_%.pas build/skj-pc | build
	./build/skj-pc -o $@ $<

## Pascal test binaries need runtime objects
PAS_BINS := $(PAS_TESTS_NOFLOAT:tests/%.pas=build/%)
$(PAS_BINS): build/%: build/%.o build/start.o build/pascal_rt.o build/skj-ld
	./build/skj-ld -o $@ build/start.o build/pascal_rt.o $<

## FPU integration test (IR builder → ColdFire asm → qemu-m68k)
FPU_TEST_SRC := tests/test_fpu.c $(IR_SRC) $(BE_CF)

build/test_fpu: $(FPU_TEST_SRC) ir/ir.h | build
	$(CC) $(CFLAGS) -Iir -o $@ $(FPU_TEST_SRC)

build/fpu_test.s: build/test_fpu | build
	./build/test_fpu -o $@

build/fpu_test.o: build/fpu_test.s
	$(M68K_AS) -o $@ $<

build/fpu_test: build/fpu_test.o build/start.o build/half.o build/skj-ld
	./build/skj-ld -o $@ build/start.o build/half.o $<

test-fpu: build/fpu_test
	@echo "Running FPU test under qemu-m68k..."
	@$(QEMU) ./build/fpu_test && echo "PASS: fpu_test" || (echo "FAIL: fpu_test"; exit 1)

## Native single-precision (IR_F32) integration test on ColdFire.  Its FPU
## computes wide, so F32 is storage-only (round on store); the test's checks
## are integer-valued, so they pass under the compute-wide model.
F32_TEST_CF_SRC := tests/test_f32.c $(IR_SRC) $(BE_CF)
build/test_f32_cf: $(F32_TEST_CF_SRC) ir/ir.h | build
	$(CC) $(CFLAGS) -Iir -o $@ $(F32_TEST_CF_SRC)
build/f32_test.s: build/test_f32_cf | build
	./build/test_f32_cf -o $@
build/f32_test.o: build/f32_test.s
	$(M68K_AS) -o $@ $<
build/f32_test: build/f32_test.o build/start.o build/skj-ld
	./build/skj-ld -o $@ build/start.o $<
test-f32: build/f32_test
	@echo "Running F32 test under qemu-m68k..."
	@$(QEMU) ./build/f32_test && echo "PASS: f32_test (ColdFire)" || (echo "FAIL: f32_test (ColdFire)"; exit 1)

## I64 integration test (IR builder → ColdFire asm → qemu-m68k)
I64_TEST_CF_SRC := tests/test_i64.c $(IR_SRC) $(BE_CF)

build/test_i64_cf: $(I64_TEST_CF_SRC) ir/ir.h | build
	$(CC) $(CFLAGS) -Iir -o $@ $(I64_TEST_CF_SRC)

build/i64_test_cf.s: build/test_i64_cf | build
	./build/test_i64_cf -o $@

build/i64_test_cf.o: build/i64_test_cf.s
	$(M68K_AS) -o $@ $<

build/i64_test_cf: build/i64_test_cf.o build/start.o build/skj-ld
	./build/skj-ld -o $@ build/start.o $<

test-i64: build/i64_test_cf
	@echo "Running I64 test under qemu-m68k..."
	@$(QEMU) ./build/i64_test_cf && echo "PASS: i64_test (ColdFire)" || (echo "FAIL: i64_test (ColdFire)"; exit 1)

## I64 integration test (IR builder → RISC-V asm → qemu-riscv32)
I64_TEST_RV_SRC := tests/test_i64.c $(IR_SRC) $(BE_RV)

build/test_i64_rv: $(I64_TEST_RV_SRC) ir/ir.h | build
	$(CC) $(CFLAGS) -Iir -o $@ $(I64_TEST_RV_SRC)

build/rv/i64_test.s: build/test_i64_rv | build/rv
	./build/test_i64_rv -o $@

build/rv/i64_test.o: build/rv/i64_test.s
	$(RV_AS) -march=rv32im -mabi=ilp32 -o $@ $<

build/rv/i64_test: build/rv/i64_test.o build/rv/start.o
	$(RV_LD) -m elf32lriscv -o $@ build/rv/start.o $<

test-i64-rv: build/rv/i64_test
	@echo "Running I64 test under qemu-riscv32..."
	@$(QEMU_RV) ./build/rv/i64_test && echo "PASS: i64_test (RISC-V)" || (echo "FAIL: i64_test (RISC-V)"; exit 1)

## Debug-info line markers (-g, item 1 of doc/tools/debugger.md): cc emits an
## IR_LOC per statement that the RISC-V backend records as a ".loc 1 N"
## directive (which the assembler turns into .debug_line, item 3).
## Assert the markers appear under -g, are absent without it, and that the -g
## assembly still assembles with skj-as-rv.
test-cc-rv-loc: build/skj-cc-rv build/skj-as-rv
	@mkdir -p build/debug
	@./build/skj-cc-rv -g -o build/debug/loc_g.s tests/debug/loc.c
	@./build/skj-cc-rv    -o build/debug/loc_n.s tests/debug/loc.c
	@g=`grep -c '\.loc' build/debug/loc_g.s`; \
	 n=`grep -c '\.loc' build/debug/loc_n.s`; \
	 if [ "$$g" -ge 4 ] && [ "$$n" -eq 0 ]; then \
	     echo "PASS: cc-rv -g line markers ($$g with -g, $$n without)"; \
	 else \
	     echo "FAIL: cc-rv -g line markers ($$g with -g, $$n without)"; exit 1; \
	 fi
	@./build/skj-as-rv -o build/debug/loc_g.o build/debug/loc_g.s \
	     && echo "PASS: cc-rv -g assembly assembles"

## Debug-info line markers (-g) on the Kobold path: kpcom emits the .file hint
## and a per-statement LOC, the KPM1 reader carries .file onto ir_func.src_file,
## and the RISC-V backend records each as a ".loc 1 N" directive.  Same
## assertions as test-cc-rv-loc.
test-kp-rv-loc: build/skj-kpc-rv build/skj-kp-rv build/skj-as-rv
	@mkdir -p build/debug
	@./build/skj-kpc-rv --emit-kpm -g -o build/debug/kloc_g.kpm tests/debug/loc.pas
	@./build/skj-kpc-rv --emit-kpm    -o build/debug/kloc_n.kpm tests/debug/loc.pas
	@./build/skj-kp-rv -o build/debug/kloc_g.s build/debug/kloc_g.kpm
	@./build/skj-kp-rv -o build/debug/kloc_n.s build/debug/kloc_n.kpm
	@g=`grep -c '\.loc' build/debug/kloc_g.s`; \
	 n=`grep -c '\.loc' build/debug/kloc_n.s`; \
	 if [ "$$g" -ge 3 ] && [ "$$n" -eq 0 ]; then \
	     echo "PASS: kp-rv -g line markers ($$g with -g, $$n without)"; \
	 else \
	     echo "FAIL: kp-rv -g line markers ($$g with -g, $$n without)"; exit 1; \
	 fi
	@./build/skj-as-rv -o build/debug/kloc_g.o build/debug/kloc_g.s \
	     && echo "PASS: kp-rv -g assembly assembles"

## Kobold DWARF end to end (the Kobold twin of test-rv-addr2line): a -g Kobold
## module compiles to a linked rv32 executable whose DWARF a stock toolchain
## reads.  skj-kpc emits the LOC under -g, skj-kp-rv (the shared rv backend, so
## .loc/.type/.size come for free) emits the directives, skj-as-rv builds the
## .debug_* sections, and skj-ld-rv relocates them and re-emits the .symtab.
## skj-addr2line must then agree with GNU addr2line on function and line for
## every statement, proving a Kobold crash or breakpoint symbolizes like a C one.
test-kp-rv-dwarf: build/skj-kpc-rv build/skj-kp-rv build/skj-as-rv build/skj-ld-rv \
                  build/skj-addr2line build/kpfree-rv/kp_crt_rv.o build/libkpaot-rv-free.a
	@mkdir -p build/debug
	@./build/skj-kpc-rv --emit-kpm -g -o build/debug/kdw.kpm tests/debug/loc.pas
	@./build/skj-kp-rv -o build/debug/kdw.s build/debug/kdw.kpm
	@./build/skj-as-rv -o build/debug/kdw.o build/debug/kdw.s
	@./build/skj-ld-rv -o build/debug/kdw.elf build/kpfree-rv/kp_crt_rv.o \
	     build/debug/kdw.o build/libkpaot-rv-free.a
	@llvm-dwarfdump --debug-line build/debug/kdw.elf > build/debug/kdw.line
	@ok=1; n=0; \
	 for addr in `awk '/ is_stmt/ && !/end_sequence/ {print $$1}' build/debug/kdw.line`; do \
	     n=`expr $$n + 1`; \
	     sf=`./build/skj-addr2line -f -e build/debug/kdw.elf $$addr | sed 's:.*/::'`; \
	     gf=`$(RV_ADDR2LINE) -f -e build/debug/kdw.elf $$addr | sed 's:.*/::'`; \
	     if [ "$$sf" != "$$gf" ]; then \
	         echo "FAIL: $$addr skj[$$sf] != gnu[$$gf]"; ok=0; \
	     fi; \
	 done; \
	 if [ "$$n" -lt 1 ]; then echo "FAIL: no statement addresses found"; ok=0; fi; \
	 if [ "$$ok" = 1 ]; then \
	     echo "PASS: Kobold -g executable symbolizes, skj-addr2line matches GNU on $$n statements"; \
	 else exit 1; fi

## A Kobold runtime fault prints a symbolized backtrace, like a C abort.  A
## failed assert() routes through the freestanding trap (rt_free.c), which calls
## the same RV_SYS_FATAL ecall C's abort uses, so under skj-run a Kobold trap
## shows a source-level stack (deep -> mid) and exits 70.  Combines item 2's
## DWARF (-g) with item 4's trap routing, so it needs both landed.
test-kp-rv-trap-backtrace: build/skj-kpc-rv build/skj-kp-rv build/skj-as-rv build/skj-ld-rv \
                           build/skj-run build/kpfree-rv/kp_crt_rv.o build/libkpaot-rv-free.a
	@mkdir -p build/debug
	@./build/skj-kpc-rv --emit-kpm -g -o build/debug/ktrap.kpm tests/debug/ktrap.pas
	@./build/skj-kp-rv -o build/debug/ktrap.s build/debug/ktrap.kpm
	@./build/skj-as-rv -o build/debug/ktrap.o build/debug/ktrap.s
	@./build/skj-ld-rv -o build/debug/ktrap.elf build/kpfree-rv/kp_crt_rv.o \
	     build/debug/ktrap.o build/libkpaot-rv-free.a
	@./build/skj-run build/debug/ktrap.elf 2>build/debug/ktrap.err; rc=$$?; \
	 [ "$$rc" = 70 ] || { echo "FAIL: trap exit $$rc (want 70)"; cat build/debug/ktrap.err; exit 1; }; \
	 grep -q "assertion failed" build/debug/ktrap.err || { echo "FAIL: no assert message"; cat build/debug/ktrap.err; exit 1; }; \
	 grep -q "backtrace:" build/debug/ktrap.err || { echo "FAIL: no backtrace"; cat build/debug/ktrap.err; exit 1; }; \
	 bt=`grep '^#' build/debug/ktrap.err | sed 's/.*0x[0-9a-f]* *//; s/ at .*//'`; \
	 order=`echo "$$bt" | grep -E 'deep|mid' | tr '\n' ' '`; \
	 if [ "$$order" = "deep mid " ]; then \
	     echo "PASS: skj-run Kobold trap backtrace walks deep -> mid, exit 70"; \
	 else \
	     echo "FAIL: Kobold trap backtrace order was [$$order]"; \
	     cat build/debug/ktrap.err; exit 1; \
	 fi

## Debug-info line markers (-g) on the Compact Pascal path: skj-pc emits an
## IR_LOC per statement and records the source file on each function; the
## ColdFire backend records each marker as a DWARF ".loc" directive.  Assert the
## markers appear under -g, are absent without it, and the -g assembly still
## assembles with skj-as.
test-pc-loc: build/skj-pc build/skj-as
	@mkdir -p build/debug
	@./build/skj-pc -g -o build/debug/pcloc_g.s tests/debug/loc_pc.pas
	@./build/skj-pc    -o build/debug/pcloc_n.s tests/debug/loc_pc.pas
	@g=`grep -c '\.loc' build/debug/pcloc_g.s`; \
	 n=`grep -c '\.loc' build/debug/pcloc_n.s`; \
	 if [ "$$g" -ge 3 ] && [ "$$n" -eq 0 ]; then \
	     echo "PASS: pc -g line markers ($$g with -g, $$n without)"; \
	 else \
	     echo "FAIL: pc -g line markers ($$g with -g, $$n without)"; exit 1; \
	 fi
	@./build/skj-as -o build/debug/pcloc_g.o build/debug/pcloc_g.s \
	     && echo "PASS: pc -g assembly assembles"

## Debug-info line markers (-g) on the MooScript path: skj-mooc emits an IR_LOC
## per statement and records the source file on each function; the ColdFire
## backend records each marker as a DWARF ".loc" directive.  Assert the markers
## appear under -g, are absent without it, and the -g assembly still assembles
## with skj-as.
test-moo-loc: build/skj-mooc build/skj-as
	@mkdir -p build/debug
	@./build/skj-mooc -g -o build/debug/mooloc_g.s tests/debug/loc.moo
	@./build/skj-mooc    -o build/debug/mooloc_n.s tests/debug/loc.moo
	@g=`grep -c '\.loc' build/debug/mooloc_g.s`; \
	 n=`grep -c '\.loc' build/debug/mooloc_n.s`; \
	 if [ "$$g" -ge 4 ] && [ "$$n" -eq 0 ]; then \
	     echo "PASS: moo -g line markers ($$g with -g, $$n without)"; \
	 else \
	     echo "FAIL: moo -g line markers ($$g with -g, $$n without)"; exit 1; \
	 fi
	@./build/skj-as -o build/debug/mooloc_g.o build/debug/mooloc_g.s \
	     && echo "PASS: moo -g assembly assembles"

## Debug-info line markers (-g) on the TinC path: skj-tinc emits an IR_LOC per
## statement and records the source file on each function; the ColdFire backend
## records each marker as a DWARF ".loc" directive.  Assert the markers appear under
## -g, are absent without it, and the -g assembly still assembles with skj-as.
test-tinc-loc: build/skj-tinc build/skj-as
	@mkdir -p build/debug
	@./build/skj-tinc -g -o build/debug/tcloc_g.s tests/debug/loc.tc
	@./build/skj-tinc    -o build/debug/tcloc_n.s tests/debug/loc.tc
	@g=`grep -c '\.loc' build/debug/tcloc_g.s`; \
	 n=`grep -c '\.loc' build/debug/tcloc_n.s`; \
	 if [ "$$g" -ge 5 ] && [ "$$n" -eq 0 ]; then \
	     echo "PASS: tinc -g line markers ($$g with -g, $$n without)"; \
	 else \
	     echo "FAIL: tinc -g line markers ($$g with -g, $$n without)"; exit 1; \
	 fi
	@./build/skj-as -o build/debug/tcloc_g.o build/debug/tcloc_g.s \
	     && echo "PASS: tinc -g assembly assembles"

## DWARF 4 line-number serializer (item 2 of doc/tools/debugger.md): the
## dwarf/ serializer turns a (code address, line) row table into a .debug_line
## program.  dwline-emit writes the section bytes for a fixed table; the bytes
## are wrapped in an ELF with GNU as (.incbin) and decoded by readelf and
## llvm-dwarfdump, which must report exactly the rows emitted.  This is the
## oracle check for the encoding, independent of skj-as-rv/skj-ld-rv (item 3).
build/dwline-emit: tests/dwarf/dwline_emit.c dwarf/dwarf.c dwarf/dwarf.h | build
	$(CC) $(CFLAGS) -Idwarf -Iir -o $@ tests/dwarf/dwline_emit.c dwarf/dwarf.c \
	    ir/arena.c ir/util.c

## skj-addr2line (item 4): the standalone address-to-source tool over the shared
## DWARF reader (dwread.c).  Reads the .debug_line table and .symtab a skj build
## carries (item 3) and answers address -> function, file:line.
build/skj-addr2line: dwarf/addr2line_main.c dwarf/dwread.c dwarf/dwread.h | build
	$(CC) $(CFLAGS) -Idwarf -Iir -o $@ dwarf/addr2line_main.c dwarf/dwread.c \
	    ir/arena.c ir/util.c

## skj-dbg (item 6): the source-level debugger.  It launches skj-run --gdb,
## connects over the GDB remote protocol, and symbolizes through the shared
## DWARF reader (dw_backtrace walks the fp chain reading memory over the wire).
build/skj-dbg: dbg/skj_dbg.c dwarf/dwread.c dwarf/dwread.h | build
	$(CC) $(CFLAGS) -Idwarf -Iir -o $@ dbg/skj_dbg.c dwarf/dwread.c \
	    ir/arena.c ir/util.c

test-dwarf-line: build/dwline-emit
	@mkdir -p build/debug
	@./build/dwline-emit build/debug
	@printf '\t.section\t.debug_abbrev,""\n\t.incbin\t"build/debug/dwabbrev.bin"\n\t.section\t.debug_info,""\n\t.incbin\t"build/debug/dwinfo.bin"\n\t.section\t.debug_line,""\n\t.incbin\t"build/debug/dwline.bin"\n' \
	    > build/debug/dwline.s
	@$(RV_AS) -march=rv32im -mabi=ilp32 -o build/debug/dwline.o build/debug/dwline.s
	@readelf --debug-dump=decodedline build/debug/dwline.o > build/debug/dwline.readelf
	@readelf --debug-dump=info build/debug/dwline.o > build/debug/dwinfo.readelf
	@llvm-dwarfdump --debug-line build/debug/dwline.o > build/debug/dwline.dwdump
	@llvm-dwarfdump --debug-info build/debug/dwline.o > build/debug/dwinfo.dwdump
	@llvm-dwarfdump --lookup=0x108 build/debug/dwline.o > build/debug/dwlookup.dwdump
	@ok=1; \
	 for pair in "10 100" "11 108" "12 110"; do \
	     set -- $$pair; ln=$$1; ah=$$2; \
	     grep -Eq "loc\.c +$$ln +0x$$ah" build/debug/dwline.readelf || { \
	         echo "FAIL: readelf missing row line $$ln at 0x$$ah"; ok=0; }; \
	     adp=`printf '0x%016x' 0x$$ah`; \
	     grep -Eq "$$adp +$$ln " build/debug/dwline.dwdump || { \
	         echo "FAIL: llvm-dwarfdump missing row line $$ln at $$adp"; ok=0; }; \
	 done; \
	 grep -q "end_sequence" build/debug/dwline.dwdump || { \
	     echo "FAIL: no end_sequence in the line program"; ok=0; }; \
	 grep -q "DW_TAG_compile_unit" build/debug/dwinfo.dwdump || { \
	     echo "FAIL: no compile_unit DIE in .debug_info"; ok=0; }; \
	 grep -Eq "DW_AT_stmt_list.*0x0*0\)" build/debug/dwinfo.dwdump || { \
	     echo "FAIL: compile_unit missing DW_AT_stmt_list -> line program"; ok=0; }; \
	 grep -q "DW_TAG_compile_unit" build/debug/dwinfo.readelf || { \
	     echo "FAIL: readelf cannot decode the compile_unit DIE"; ok=0; }; \
	 grep -Eq "line 11" build/debug/dwlookup.dwdump || { \
	     echo "FAIL: address lookup of 0x108 did not resolve to line 11"; ok=0; }; \
	 if [ "$$ok" = 1 ]; then \
	     echo "PASS: dwarf .debug_{line,info,abbrev} decode and attach; 0x108 resolves to loc.c:11"; \
	 else exit 1; fi

## STT_FUNC symbols with sizes (item 2 of doc/tools/debugger.md): the RISC-V
## backend emits .type @function and .size per function, and skj-as-rv honors
## them into the object .symtab.  A debugger's address-to-function map and a
## backtrace read these.  Assemble tests/debug/funcs.c with both skj-as-rv and
## GNU as and check each function is a sized FUNC symbol with the same size.
test-rv-stt-func: build/skj-cc-rv build/skj-as-rv
	@mkdir -p build/debug
	@./build/skj-cc-rv -o build/debug/funcs.s tests/debug/funcs.c
	@./build/skj-as-rv -o build/debug/funcs.skj.o build/debug/funcs.s
	@$(RV_AS) -march=rv32im -mabi=ilp32 -o build/debug/funcs.gas.o build/debug/funcs.s
	@readelf -s build/debug/funcs.skj.o > build/debug/funcs.skj.syms
	@readelf -s build/debug/funcs.gas.o > build/debug/funcs.gas.syms
	@ok=1; \
	 for fn in add helper main; do \
	     ss=`awk -v f=$$fn '$$4=="FUNC" && $$8==f {print $$3}' build/debug/funcs.skj.syms`; \
	     gs=`awk -v f=$$fn '$$4=="FUNC" && $$8==f {print $$3}' build/debug/funcs.gas.syms`; \
	     if [ -z "$$ss" ]; then \
	         echo "FAIL: $$fn is not a FUNC symbol in skj-as-rv output"; ok=0; \
	     elif [ "$$ss" = 0 ]; then \
	         echo "FAIL: $$fn has zero st_size in skj-as-rv output"; ok=0; \
	     elif [ "$$ss" != "$$gs" ]; then \
	         echo "FAIL: $$fn size $$ss (skj) != $$gs (gas)"; ok=0; \
	     fi; \
	 done; \
	 if [ "$$ok" = 1 ]; then \
	     echo "PASS: skj-as-rv emits sized STT_FUNC symbols matching GNU as"; \
	 else exit 1; fi

## DWARF debug sections from .loc (item 3 of doc/tools/debugger.md): the RISC-V
## backend emits .file/.loc under -g; skj-as-rv collects the rows and builds
## .debug_line/.debug_info/.debug_abbrev (dwarf/), with R_RISCV_32 at the
## absolute address fields.  Validate on the object: the sections exist, the CU
## carries DW_AT_stmt_list, the line program ends a sequence, and the decoded
## (address, line) rows match GNU as fed the identical .loc stream.  Full
## address-to-line resolution on a linked executable is the linker slice.
test-rv-debug-sections: build/skj-cc-rv build/skj-as-rv
	@mkdir -p build/debug
	@./build/skj-cc-rv -g -o build/debug/dsec.s tests/debug/loc.c
	@./build/skj-as-rv -o build/debug/dsec.skj.o build/debug/dsec.s
	@$(RV_AS) -march=rv32im -mabi=ilp32 -o build/debug/dsec.gas.o build/debug/dsec.s
	@readelf -S build/debug/dsec.skj.o > build/debug/dsec.secs
	@llvm-dwarfdump --debug-info build/debug/dsec.skj.o > build/debug/dsec.info
	@llvm-dwarfdump --debug-line build/debug/dsec.skj.o > build/debug/dsec.line
	@llvm-dwarfdump --debug-line build/debug/dsec.gas.o > build/debug/dsec.gasline
	@ok=1; \
	 for s in .debug_line .debug_info .debug_abbrev; do \
	     grep -q "$$s" build/debug/dsec.secs || { \
	         echo "FAIL: section $$s missing from skj-as-rv object"; ok=0; }; \
	 done; \
	 grep -q "DW_TAG_compile_unit" build/debug/dsec.info || { \
	     echo "FAIL: no compile_unit DIE"; ok=0; }; \
	 grep -q "DW_AT_stmt_list" build/debug/dsec.info || { \
	     echo "FAIL: CU missing DW_AT_stmt_list"; ok=0; }; \
	 grep -q "end_sequence" build/debug/dsec.line || { \
	     echo "FAIL: line program has no end_sequence"; ok=0; }; \
	 sl=`awk '/ is_stmt/{print $$2}' build/debug/dsec.line | tr '\n' ' '`; \
	 gl=`awk '/ is_stmt/{print $$2}' build/debug/dsec.gasline | tr '\n' ' '`; \
	 if [ "$$sl" != "$$gl" ]; then \
	     echo "FAIL: line rows [$$sl] != GNU as [$$gl]"; ok=0; \
	 fi; \
	 if [ "$$ok" = 1 ]; then \
	     echo "PASS: skj-as-rv builds .debug_{line,info,abbrev} from .loc (rows match GNU as: $$sl)"; \
	 else exit 1; fi

## DWARF through the linker (item 3, the linker slice): skj-ld-rv preserves the
## .debug_* sections, relocates their absolute address fields to final link
## addresses, and emits them in the executable.  The payoff is addr2line and
## llvm-dwarfdump --lookup resolving a real address to file:line on a
## skj-linked binary, which the GNU toolchain's addr2line confirms.  Also check
## the executable still runs (the debug sections are non-alloc, so they must not
## perturb the loadable image) and matches the exit the non-debug build gives.
test-rv-debug-link: build/skj-cc-rv build/skj-as-rv build/skj-ld-rv build/rv/start.o
	@mkdir -p build/debug
	@./build/skj-cc-rv -g -o build/debug/dlink.s tests/debug/loc.c
	@./build/skj-as-rv -o build/debug/dlink.o build/debug/dlink.s
	@./build/skj-ld-rv -o build/debug/dlink.elf build/rv/start.o build/debug/dlink.o
	@readelf -S build/debug/dlink.elf > build/debug/dlink.secs
	@readelf -s build/debug/dlink.elf > build/debug/dlink.syms
	@llvm-dwarfdump --debug-line build/debug/dlink.elf > build/debug/dlink.line
	@ok=1; \
	 for s in .debug_line .debug_info .debug_abbrev .symtab; do \
	     grep -q "$$s" build/debug/dlink.secs || { \
	         echo "FAIL: section $$s missing from linked executable"; ok=0; }; \
	 done; \
	 msz=`awk '$$4=="FUNC" && $$8=="main" {print $$3}' build/debug/dlink.syms`; \
	 if [ -z "$$msz" ] || [ "$$msz" = 0 ]; then \
	     echo "FAIL: main is not a sized FUNC symbol in .symtab"; ok=0; \
	 fi; \
	 $(QEMU_RV) ./build/debug/dlink.elf; ec=$$?; \
	 if [ $$ec -ne 5 ]; then \
	     echo "FAIL: linked -g executable exited $$ec (expected 5)"; ok=0; \
	 fi; \
	 addr=`awk '/ is_stmt/{print $$1; exit}' build/debug/dlink.line`; \
	 line=`awk '/ is_stmt/{print $$2; exit}' build/debug/dlink.line`; \
	 res=`$(RV_ADDR2LINE) -f -e build/debug/dlink.elf $$addr | tr '\n' ' '`; \
	 case "$$res" in \
	     "??"*|"") echo "FAIL: addr2line -f gave no function for $$addr"; ok=0 ;; \
	     *loc.c:$$line*) : ;; \
	     *) echo "FAIL: addr2line -f $$addr -> $$res (expected loc.c:$$line)"; ok=0 ;; \
	 esac; \
	 if [ "$$ok" = 1 ]; then \
	     echo "PASS: skj-ld-rv carries DWARF + .symtab; addr2line -f $$addr -> $$res, runs (exit 5)"; \
	 else exit 1; fi

## skj-addr2line over the shared DWARF reader (item 4): resolve every compiled
## statement address on a skj-linked executable and check the function name and
## source line agree with GNU addr2line.  Paths are compared by basename, since
## skj prints the stored relative name where GNU prepends the CU comp_dir.
test-rv-addr2line: build/skj-cc-rv build/skj-as-rv build/skj-ld-rv build/skj-addr2line build/rv/start.o
	@mkdir -p build/debug
	@./build/skj-cc-rv -g -o build/debug/a2l.s tests/debug/loc.c
	@./build/skj-as-rv -o build/debug/a2l.o build/debug/a2l.s
	@./build/skj-ld-rv -o build/debug/a2l.elf build/rv/start.o build/debug/a2l.o
	@llvm-dwarfdump --debug-line build/debug/a2l.elf > build/debug/a2l.line
	@ok=1; n=0; \
	 for addr in `awk '/ is_stmt/ && !/end_sequence/ {print $$1}' build/debug/a2l.line`; do \
	     n=`expr $$n + 1`; \
	     sf=`./build/skj-addr2line -f -e build/debug/a2l.elf $$addr | sed 's:.*/::'`; \
	     gf=`$(RV_ADDR2LINE) -f -e build/debug/a2l.elf $$addr | sed 's:.*/::'`; \
	     if [ "$$sf" != "$$gf" ]; then \
	         echo "FAIL: $$addr skj[$$sf] != gnu[$$gf]"; ok=0; \
	     fi; \
	 done; \
	 if [ "$$n" -lt 1 ]; then echo "FAIL: no statement addresses found"; ok=0; fi; \
	 if [ "$$ok" = 1 ]; then \
	     echo "PASS: skj-addr2line matches GNU addr2line on $$n statements (function + line)"; \
	 else exit 1; fi

## The ColdFire twin of test-rv-addr2line: the skj-cc / skj-as / skj-ld path
## emits big-endian DWARF through to a linked m68k executable, and skj-addr2line
## must agree with GNU addr2line on function and line for every statement.
test-cf-addr2line: build/skj-cc build/skj-as build/skj-ld build/skj-addr2line build/start.o
	@mkdir -p build/debug
	@./build/skj-cc -g -o build/debug/cfa2l.s tests/debug/loc.c
	@./build/skj-as -o build/debug/cfa2l.o build/debug/cfa2l.s
	@./build/skj-ld -o build/debug/cfa2l.elf build/start.o build/debug/cfa2l.o
	@llvm-dwarfdump --debug-line build/debug/cfa2l.elf > build/debug/cfa2l.line
	@ok=1; n=0; \
	 for addr in `awk '/ is_stmt/ && !/end_sequence/ {print $$1}' build/debug/cfa2l.line`; do \
	     n=`expr $$n + 1`; \
	     sf=`./build/skj-addr2line -f -e build/debug/cfa2l.elf $$addr | sed 's:.*/::'`; \
	     gf=`$(M68K_ADDR2LINE) -f -e build/debug/cfa2l.elf $$addr | sed 's:.*/::'`; \
	     if [ "$$sf" != "$$gf" ]; then \
	         echo "FAIL: $$addr skj[$$sf] != gnu[$$gf]"; ok=0; \
	     fi; \
	 done; \
	 if [ "$$n" -lt 1 ]; then echo "FAIL: no statement addresses found"; ok=0; fi; \
	 if [ "$$ok" = 1 ]; then \
	     echo "PASS: skj-addr2line matches GNU addr2line on $$n ColdFire statements (function + line)"; \
	 else exit 1; fi

## The ColdFire twin of test-rv-wild-backtrace: a call through a garbage pointer
## faults with a wild pc, so skj-run starts its backtrace from a bogus top frame
## and must still unwind the intact a6 chain below it (to main) and exit 139
## (SIGSEGV), proving the in-emulator ColdFire fault backtrace (emu/cf_user.c).
test-cf-wild-backtrace: build/skj-cc build/skj-as build/skj-ld build/skj-run build/start.o
	@mkdir -p build/debug
	@./build/skj-cc -g -o build/debug/cfwild.s tests/debug/wildjump.c
	@./build/skj-as -o build/debug/cfwild.o build/debug/cfwild.s
	@./build/skj-ld -o build/debug/cfwild.elf build/start.o build/debug/cfwild.o
	@./build/skj-run build/debug/cfwild.elf 2>build/debug/cfwild.err; rc=$$?; \
	 [ "$$rc" = 139 ] || { echo "FAIL: wild jump exit $$rc (want 139)"; cat build/debug/cfwild.err; exit 1; }; \
	 grep -q "backtrace:" build/debug/cfwild.err || { echo "FAIL: no backtrace"; cat build/debug/cfwild.err; exit 1; }; \
	 grep -qE '  main( at |$$)' build/debug/cfwild.err || { echo "FAIL: chain below the wild frame not unwound"; cat build/debug/cfwild.err; exit 1; }; \
	 echo "PASS: skj-run survives a wild ColdFire frame, unwinds to main, exit 139"

## The ColdFire twin of test-rv-abort-backtrace: a failed assert calls abort(),
## which issues the CF_SYS_FATAL trap (__skj_trap_backtrace); skj-run symbolizes
## the a6 chain and returns, and the guest exits 134 (128 + SIGABRT).  Exercises
## the controlled-trap backtrace path through the C libc.
test-cf-abort-backtrace: build/skj-cc build/skj-as build/skj-ld build/skj-run \
                         build/lib/coldfire/crt0.o build/lib/coldfire/va_cf.o \
                         build/lib/coldfire/fini.o build/lib/coldfire/libc.a
	@mkdir -p build/debug
	@./build/skj-cc -g -Iinclude -o build/debug/cfaborts.s tests/debug/aborts.c
	@./build/skj-as -o build/debug/cfaborts.o build/debug/cfaborts.s
	@./build/skj-ld -o build/debug/cfaborts.elf build/lib/coldfire/crt0.o \
	     build/debug/cfaborts.o build/lib/coldfire/va_cf.o \
	     build/lib/coldfire/fini.o build/lib/coldfire/libc.a
	@./build/skj-run build/debug/cfaborts.elf 2>build/debug/cfaborts.err; rc=$$?; \
	 [ "$$rc" = 134 ] || { echo "FAIL: abort exit $$rc (want 134)"; cat build/debug/cfaborts.err; exit 1; }; \
	 grep -q "assertion failed" build/debug/cfaborts.err || { echo "FAIL: no assert message"; cat build/debug/cfaborts.err; exit 1; }; \
	 grep -q "backtrace:" build/debug/cfaborts.err || { echo "FAIL: no backtrace"; cat build/debug/cfaborts.err; exit 1; }; \
	 bt=`grep '^#' build/debug/cfaborts.err | sed 's/.*0x[0-9a-f]* *//; s/ at .*//'`; \
	 order=`echo "$$bt" | grep -E 'deep|mid|main' | tr '\n' ' '`; \
	 if [ "$$order" = "deep mid main " ]; then \
	     echo "PASS: skj-run ColdFire abort backtrace walks deep -> mid -> main, exit 134"; \
	 else \
	     echo "FAIL: abort backtrace order was [$$order]"; \
	     cat build/debug/cfaborts.err; exit 1; \
	 fi

## The ColdFire twin of test-kp-rv-dwarf: a -g Kobold module compiles to a linked
## m68k executable whose DWARF a stock toolchain reads.  skj-kp-cf shares the cf
## backend (so .loc/.type/.size come for free), skj-as builds the big-endian
## .debug_* sections (R_68K_32), and skj-ld relocates them; skj-addr2line must
## then agree with GNU addr2line on function and line for every statement.
test-kp-cf-dwarf: build/skj-kpc-cf build/skj-kp-cf build/skj-as build/skj-ld \
                  build/skj-addr2line build/kpfree-cf/kp_crt_cf.o build/libkpaot-cf-free.a
	@mkdir -p build/debug
	@./build/skj-kpc-cf --emit-kpm -g -o build/debug/kdwcf.kpm tests/debug/loc.pas
	@./build/skj-kp-cf -o build/debug/kdwcf.s build/debug/kdwcf.kpm
	@./build/skj-as -o build/debug/kdwcf.o build/debug/kdwcf.s
	@./build/skj-ld -o build/debug/kdwcf.elf build/kpfree-cf/kp_crt_cf.o \
	     build/debug/kdwcf.o build/libkpaot-cf-free.a
	@llvm-dwarfdump --debug-line build/debug/kdwcf.elf > build/debug/kdwcf.line
	@ok=1; n=0; \
	 for addr in `awk '/ is_stmt/ && !/end_sequence/ {print $$1}' build/debug/kdwcf.line`; do \
	     n=`expr $$n + 1`; \
	     sf=`./build/skj-addr2line -f -e build/debug/kdwcf.elf $$addr | sed 's:.*/::'`; \
	     gf=`$(M68K_ADDR2LINE) -f -e build/debug/kdwcf.elf $$addr | sed 's:.*/::'`; \
	     if [ "$$sf" != "$$gf" ]; then \
	         echo "FAIL: $$addr skj[$$sf] != gnu[$$gf]"; ok=0; \
	     fi; \
	 done; \
	 if [ "$$n" -lt 1 ]; then echo "FAIL: no statement addresses found"; ok=0; fi; \
	 if [ "$$ok" = 1 ]; then \
	     echo "PASS: Kobold -g ColdFire executable symbolizes, skj-addr2line matches GNU on $$n statements"; \
	 else exit 1; fi

## The ColdFire twin of test-kp-rv-trap-backtrace: a Kobold runtime fault routes
## through the freestanding trap (rt_free.c), which calls __skj_trap_backtrace
## (the CF_SYS_FATAL trap); under skj-run it shows a source-level stack
## (deep -> mid) and exits 70.  Exercises the controlled-trap path end to end.
test-kp-cf-trap-backtrace: build/skj-kpc-cf build/skj-kp-cf build/skj-as build/skj-ld \
                           build/skj-run build/kpfree-cf/kp_crt_cf.o build/libkpaot-cf-free.a
	@mkdir -p build/debug
	@./build/skj-kpc-cf --emit-kpm -g -o build/debug/ktrapcf.kpm tests/debug/ktrap.pas
	@./build/skj-kp-cf -o build/debug/ktrapcf.s build/debug/ktrapcf.kpm
	@./build/skj-as -o build/debug/ktrapcf.o build/debug/ktrapcf.s
	@./build/skj-ld -o build/debug/ktrapcf.elf build/kpfree-cf/kp_crt_cf.o \
	     build/debug/ktrapcf.o build/libkpaot-cf-free.a
	@./build/skj-run build/debug/ktrapcf.elf 2>build/debug/ktrapcf.err; rc=$$?; \
	 [ "$$rc" = 70 ] || { echo "FAIL: trap exit $$rc (want 70)"; cat build/debug/ktrapcf.err; exit 1; }; \
	 grep -q "assertion failed" build/debug/ktrapcf.err || { echo "FAIL: no assert message"; cat build/debug/ktrapcf.err; exit 1; }; \
	 grep -q "backtrace:" build/debug/ktrapcf.err || { echo "FAIL: no backtrace"; cat build/debug/ktrapcf.err; exit 1; }; \
	 bt=`grep '^#' build/debug/ktrapcf.err | sed 's/.*0x[0-9a-f]* *//; s/ at .*//'`; \
	 order=`echo "$$bt" | grep -E 'deep|mid' | tr '\n' ' '`; \
	 if [ "$$order" = "deep mid " ]; then \
	     echo "PASS: skj-run Kobold ColdFire trap backtrace walks deep -> mid, exit 70"; \
	 else \
	     echo "FAIL: Kobold trap backtrace order was [$$order]"; \
	     cat build/debug/ktrapcf.err; exit 1; \
	 fi

## The MIPS twin of test-rv-addr2line: skj-cc-mips / skj-as-mips / skj-ld-mips
## emit little-endian DWARF (REL relocations) through to a linked mipsel
## executable, and skj-addr2line must agree with GNU addr2line on every
## statement's function and line.
test-mips-addr2line: build/skj-cc-mips build/skj-as-mips build/skj-ld-mips build/skj-addr2line build/mips/start.o
	@mkdir -p build/debug
	@./build/skj-cc-mips -g -o build/debug/ma2l.s tests/debug/loc.c
	@./build/skj-as-mips -o build/debug/ma2l.o build/debug/ma2l.s
	@./build/skj-ld-mips -o build/debug/ma2l.elf build/mips/start.o build/debug/ma2l.o
	@llvm-dwarfdump --debug-line build/debug/ma2l.elf > build/debug/ma2l.line
	@ok=1; n=0; \
	 for addr in `awk '/ is_stmt/ && !/end_sequence/ {print $$1}' build/debug/ma2l.line`; do \
	     n=`expr $$n + 1`; \
	     sf=`./build/skj-addr2line -f -e build/debug/ma2l.elf $$addr | sed 's:.*/::'`; \
	     gf=`$(MIPS_ADDR2LINE) -f -e build/debug/ma2l.elf $$addr | sed 's:.*/::'`; \
	     if [ "$$sf" != "$$gf" ]; then \
	         echo "FAIL: $$addr skj[$$sf] != gnu[$$gf]"; ok=0; \
	     fi; \
	 done; \
	 if [ "$$n" -lt 1 ]; then echo "FAIL: no statement addresses found"; ok=0; fi; \
	 if [ "$$ok" = 1 ]; then \
	     echo "PASS: skj-addr2line matches GNU addr2line on $$n MIPS statements (function + line)"; \
	 else exit 1; fi

## skj-run fault backtrace (item 5): a nested call chain faults through a null
## read, and skj-run walks the frame-pointer chain and symbolizes it with the
## shared DWARF reader.  Check the frames name the functions in call order
## (level3 inner, main outer); the crt _start frame is NOTYPE and prints ??.
test-rv-backtrace: build/skj-cc-rv build/skj-as-rv build/skj-ld-rv build/skj-run build/rv/start.o
	@mkdir -p build/debug
	@./build/skj-cc-rv -g -o build/debug/crash.s tests/debug/crash.c
	@./build/skj-as-rv -o build/debug/crash.o build/debug/crash.s
	@./build/skj-ld-rv -o build/debug/crash.elf build/rv/start.o build/debug/crash.o
	@./build/skj-run build/debug/crash.elf 2>build/debug/crash.err; \
	 bt=`grep '^#' build/debug/crash.err | sed 's/.*0x[0-9a-f]* *//; s/ at .*//'`; \
	 order=`echo "$$bt" | grep -E 'level3|level2|level1|main' | tr '\n' ' '`; \
	 if [ "$$order" = "level3 level2 level1 main " ]; then \
	     echo "PASS: skj-run backtrace walks level3 -> level2 -> level1 -> main"; \
	 else \
	     echo "FAIL: backtrace order was [$$order]"; \
	     cat build/debug/crash.err; exit 1; \
	 fi

## An abort/assert prints the same symbolized backtrace as a hardware fault.
## A failed assert calls abort(), which issues the skj-run RV_SYS_FATAL ecall:
## the host walks the frame-pointer chain and symbolizes it before exiting 134,
## so an intentional abort shows a source-level stack, not just a bare message.
## The exit code must stay 134 and the backtrace must name the user frames in
## call order (deep -> mid -> main).  Needs the rv libc for abort/assert.
test-rv-abort-backtrace: build/skj-cc-rv build/skj-as-rv build/skj-ld-rv build/skj-run \
                         build/rv/start.o build/rv/va_rv.o build/rv/soft64_div.o \
                         build/rv/co_switch.o build/lib/riscv/libc.a
	@mkdir -p build/debug
	@./build/skj-cc-rv -g -Iinclude -o build/debug/aborts.s tests/debug/aborts.c
	@./build/skj-as-rv -o build/debug/aborts.o build/debug/aborts.s
	@./build/skj-ld-rv -o build/debug/aborts.elf build/rv/start.o build/debug/aborts.o \
	     build/lib/riscv/libc.a build/rv/va_rv.o build/rv/soft64_div.o build/rv/co_switch.o
	@./build/skj-run build/debug/aborts.elf 2>build/debug/aborts.err; rc=$$?; \
	 [ "$$rc" = 134 ] || { echo "FAIL: abort exit $$rc (want 134)"; cat build/debug/aborts.err; exit 1; }; \
	 grep -q "assertion failed" build/debug/aborts.err || { echo "FAIL: no assert message"; cat build/debug/aborts.err; exit 1; }; \
	 grep -q "backtrace:" build/debug/aborts.err || { echo "FAIL: no backtrace"; cat build/debug/aborts.err; exit 1; }; \
	 bt=`grep '^#' build/debug/aborts.err | sed 's/.*0x[0-9a-f]* *//; s/ at .*//'`; \
	 order=`echo "$$bt" | grep -E 'deep|mid|main' | tr '\n' ' '`; \
	 if [ "$$order" = "deep mid main " ]; then \
	     echo "PASS: skj-run abort backtrace walks deep -> mid -> main, exit 134"; \
	 else \
	     echo "FAIL: abort backtrace order was [$$order]"; \
	     cat build/debug/aborts.err; exit 1; \
	 fi

## dw_backtrace termination guards (host unit test): the backtrace walks a guest
## frame-pointer chain through a read callback, so a corrupt or looping chain
## must be bounded by the host.  Drive dw_backtrace with synthetic memory models
## (an unbounded climb, a self-referential cycle, an unreadable chain, a finite
## chain) and check it stops at the depth cap and the monotonic-fp check rather
## than spinning.  Pure host: no DWARF, ELF, or qemu.
build/bt_loop: tests/debug/bt_loop.c dwarf/dwread.c dwarf/dwread.h ir/arena.c ir/util.c | build
	$(CC) $(CFLAGS) -Idwarf -Iir -o $@ tests/debug/bt_loop.c dwarf/dwread.c \
	    ir/arena.c ir/util.c
test-bt-loop: build/bt_loop
	@./build/bt_loop

build/bt_trim: tests/debug/bt_trim.c dwarf/dwread.c dwarf/dwread.h ir/arena.c ir/util.c | build
	$(CC) $(CFLAGS) -Idwarf -Iir -o $@ tests/debug/bt_trim.c dwarf/dwread.c \
	    ir/arena.c ir/util.c

test-bt-trim: build/bt_trim
	@./build/bt_trim

## A wild frame does not crash the backtrace.  A call through a garbage function
## pointer faults with a wild pc, so skj-run's backtrace starts from a bogus top
## frame; it must still unwind the intact chain below (wrap, main) and exit 139
## (SIGSEGV), never crash or hang the host.  The integration twin of test-bt-loop.
test-rv-wild-backtrace: build/skj-cc-rv build/skj-as-rv build/skj-ld-rv build/skj-run build/rv/start.o
	@mkdir -p build/debug
	@./build/skj-cc-rv -g -o build/debug/wildjump.s tests/debug/wildjump.c
	@./build/skj-as-rv -o build/debug/wildjump.o build/debug/wildjump.s
	@./build/skj-ld-rv -o build/debug/wildjump.elf build/rv/start.o build/debug/wildjump.o
	@./build/skj-run build/debug/wildjump.elf 2>build/debug/wildjump.err; rc=$$?; \
	 [ "$$rc" = 139 ] || { echo "FAIL: wild jump exit $$rc (want 139)"; cat build/debug/wildjump.err; exit 1; }; \
	 grep -q "backtrace:" build/debug/wildjump.err || { echo "FAIL: no backtrace"; cat build/debug/wildjump.err; exit 1; }; \
	 grep -qE '  main( at |$$)' build/debug/wildjump.err || { echo "FAIL: chain below the wild frame not unwound"; cat build/debug/wildjump.err; exit 1; }; \
	 echo "PASS: skj-run survives a wild frame, unwinds to main, exit 139"

## skj-run GDB stub (item 6a): drive the --gdb RSP server with gdb-multiarch as
## the oracle.  A break at main, a backtrace at a fault (SIGSEGV), and the clean
## exit code all come back through the stub's registers/memory/breakpoints.
## Skips cleanly when gdb-multiarch is not installed (it is an extra oracle, not
## a build dependency); the self-contained check is test-rv-dbg (skj-dbg).
RV_GDB ?= gdb-multiarch
test-rv-gdb: build/skj-cc-rv build/skj-as-rv build/skj-ld-rv build/skj-run build/rv/start.o
	@if ! command -v $(RV_GDB) >/dev/null 2>&1; then \
	     echo "SKIP: test-rv-gdb ($(RV_GDB) not found)"; exit 0; fi; \
	 mkdir -p build/debug; \
	 ./build/skj-cc-rv -g -o build/debug/loc.s tests/debug/loc.c; \
	 ./build/skj-as-rv -o build/debug/loc.o build/debug/loc.s; \
	 ./build/skj-ld-rv -o build/debug/loc.elf build/rv/start.o build/debug/loc.o; \
	 ./build/skj-cc-rv -g -o build/debug/crash.s tests/debug/crash.c; \
	 ./build/skj-as-rv -o build/debug/crash.o build/debug/crash.s; \
	 ./build/skj-ld-rv -o build/debug/crash.elf build/rv/start.o build/debug/crash.o; \
	 port=$$((20000 + $$$$ % 20000)); \
	 ./build/skj-run --gdb $$port build/debug/loc.elf >/dev/null 2>build/debug/g1.err & rp=$$!; \
	 for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do \
	     grep -q listening build/debug/g1.err 2>/dev/null && break; sleep 0.2; done; \
	 $(RV_GDB) -q -nx -batch -ex "set pagination off" \
	     -ex "target remote 127.0.0.1:$$port" -ex "break main" \
	     -ex "continue" -ex "continue" build/debug/loc.elf \
	     >build/debug/g1.out 2>&1; \
	 wait $$rp 2>/dev/null; \
	 port=$$((20000 + ($$$$ + 1) % 20000)); \
	 ./build/skj-run --gdb $$port build/debug/crash.elf >/dev/null 2>build/debug/g2.err & rp=$$!; \
	 for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do \
	     grep -q listening build/debug/g2.err 2>/dev/null && break; sleep 0.2; done; \
	 $(RV_GDB) -q -nx -batch -ex "set pagination off" \
	     -ex "target remote 127.0.0.1:$$port" -ex "continue" \
	     -ex "backtrace" build/debug/crash.elf >build/debug/g2.out 2>&1; \
	 kill $$rp 2>/dev/null; wait $$rp 2>/dev/null; \
	 ok=1; \
	 grep -q "Breakpoint 1, main" build/debug/g1.out || { echo "FAIL: no break at main"; ok=0; }; \
	 grep -q "exited with code 05" build/debug/g1.out || { echo "FAIL: loc did not exit 05"; ok=0; }; \
	 grep -q "SIGSEGV" build/debug/g2.out || { echo "FAIL: no SIGSEGV on crash"; ok=0; }; \
	 for fn in level3 level2 level1 main; do \
	     grep -q "in $$fn " build/debug/g2.out || { echo "FAIL: backtrace missing $$fn"; ok=0; }; done; \
	 if [ $$ok = 1 ]; then \
	     echo "PASS: gdb-multiarch drives the skj-run RSP stub (break, fault backtrace, exit)"; \
	 else cat build/debug/g1.out build/debug/g2.out; exit 1; fi

## skj-dbg end to end (item 6b): the in-tree debugger launches skj-run --gdb,
## connects over the RSP, and drives break / continue / backtrace, symbolizing
## through the shared DWARF reader.  Self-contained (no external gdb).
test-rv-dbg: build/skj-cc-rv build/skj-as-rv build/skj-ld-rv build/skj-run build/skj-dbg build/rv/start.o
	@mkdir -p build/debug
	@./build/skj-cc-rv -g -o build/debug/loc.s tests/debug/loc.c
	@./build/skj-as-rv -o build/debug/loc.o build/debug/loc.s
	@./build/skj-ld-rv -o build/debug/loc.elf build/rv/start.o build/debug/loc.o
	@./build/skj-cc-rv -g -o build/debug/crash.s tests/debug/crash.c
	@./build/skj-as-rv -o build/debug/crash.o build/debug/crash.s
	@./build/skj-ld-rv -o build/debug/crash.elf build/rv/start.o build/debug/crash.o
	@./build/skj-dbg --run ./build/skj-run -ex "break main" -ex "continue" \
	     build/debug/loc.elf >build/debug/dbg1.out 2>&1
	@./build/skj-dbg --run ./build/skj-run -ex "continue" -ex "backtrace" \
	     build/debug/crash.elf >build/debug/dbg2.out 2>&1
	@ok=1; \
	 grep -q "breakpoint at .*main at .*loc.c:" build/debug/dbg1.out || { echo "FAIL: break main not symbolized"; ok=0; }; \
	 grep -q "stopped in main at .*loc.c:" build/debug/dbg1.out || { echo "FAIL: did not stop in main"; ok=0; }; \
	 grep -q "SIGSEGV.*in level3 at .*crash.c:" build/debug/dbg2.out || { echo "FAIL: no symbolized SIGSEGV"; ok=0; }; \
	 bt=`grep '^#' build/debug/dbg2.out | sed 's/.*0x[0-9a-f]* *//; s/ at .*//' | grep -E 'level|main' | tr '\n' ' '`; \
	 if [ "$$bt" != "level3 level2 level1 main " ]; then echo "FAIL: bt order [$$bt]"; ok=0; fi; \
	 if [ $$ok = 1 ]; then \
	     echo "PASS: skj-dbg break/continue/backtrace over the RSP, DWARF-symbolized"; \
	 else cat build/debug/dbg1.out build/debug/dbg2.out; exit 1; fi

## Sandbox file I/O (openat): a guest opens a real host file through the RV32
## sandbox's openat/read/write/close and copies it to stdout.  This is the
## first file-I/O step toward running skj-cc/Kobold inside the sandbox.  The
## skj-run output must match the fixture and qemu-riscv32 (its native openat),
## and a missing file must fail with exit 2.
test-rv-sandbox-io: build/skj-cc-rv build/skj-as-rv build/skj-ld-rv build/skj-run build/rv/start.o
	@mkdir -p build/sandbox-io
	@printf 'hello from the sandbox openat\nsecond line\n' > build/sandbox-io/greeting.txt
	@./build/skj-cc-rv -DPATH='"build/sandbox-io/greeting.txt"' -o build/sandbox-io/cat.s tests/sandbox-io/cat.c
	@./build/skj-as-rv -o build/sandbox-io/cat.o build/sandbox-io/cat.s
	@./build/skj-ld-rv -o build/sandbox-io/cat.elf build/rv/start.o build/sandbox-io/cat.o
	@./build/skj-cc-rv -DPATH='"build/sandbox-io/nope.txt"' -o build/sandbox-io/catm.s tests/sandbox-io/cat.c
	@./build/skj-as-rv -o build/sandbox-io/catm.o build/sandbox-io/catm.s
	@./build/skj-ld-rv -o build/sandbox-io/catm.elf build/rv/start.o build/sandbox-io/catm.o
	@./build/skj-run build/sandbox-io/cat.elf > build/sandbox-io/out.skj 2>/dev/null
	@$(QEMU_RV) build/sandbox-io/cat.elf > build/sandbox-io/out.qemu 2>/dev/null
	@rc=0; ./build/skj-run build/sandbox-io/catm.elf >/dev/null 2>&1 || rc=$$?; \
	 ok=1; \
	 cmp -s build/sandbox-io/out.skj build/sandbox-io/greeting.txt || { echo "FAIL: skj-run output != fixture"; ok=0; }; \
	 cmp -s build/sandbox-io/out.skj build/sandbox-io/out.qemu || { echo "FAIL: skj-run != qemu-riscv32"; ok=0; }; \
	 if [ "$$rc" != 2 ]; then echo "FAIL: missing file gave exit $$rc, expected 2"; ok=0; fi; \
	 if [ $$ok = 1 ]; then \
	     echo "PASS: sandbox openat/read/write/close (skj-run matches fixture + qemu; missing file -> exit 2)"; \
	 else exit 1; fi

## The driver's per-target runtime set (crt + helpers + libc), the objects the
## skjcc link and the self-host tests pass to the linker.  Defined here, above
## the first use: a Make prerequisite list is expanded as the rule is read, so a
## variable referenced before its definition expands empty and the stale crt0.o
## it should have rebuilt is used as-is.
DRIVER_LIB_CF   := build/lib/coldfire/crt0.o build/lib/coldfire/half.o build/lib/coldfire/va_cf.o build/lib/coldfire/fini.o build/lib/coldfire/libc.a
DRIVER_LIB_RV   := build/lib/riscv/crt0.o build/lib/riscv/va_rv.o build/lib/riscv/soft64_div.o build/lib/riscv/fini.o build/lib/riscv/libc.a
DRIVER_LIB_MIPS := build/lib/mips/crt0.o build/lib/mips/half.o build/lib/mips/va_mips.o build/lib/mips/soft64_div.o build/lib/mips/fini.o build/lib/mips/libc.a

## Guest libc file I/O + argv (continuing the sandbox openat work): a C program
## built through the skjcc driver (crt + libc) slurps an input file with the
## fopen/fseek/ftell/rewind/fread idiom the toolchain's slurp uses, writes it
## back with fopen/fwrite, and echoes argc and the paths.  Run under skj-run with
## argv; the round-trip output and the stdout must agree with the same program
## built by the host compiler and run natively, which pins the crt argv setup,
## the open/close/llseek syscall wrappers, and skj-run's openat/llseek against the
## host as the reference, with no second emulation.
test-rv-libc-io: build/skjcc $(DRIVER_LIB_RV) build/skj-run
	@mkdir -p build/libc-io
	@printf 'line one\nline two\nthe quick brown fox\n' > build/libc-io/in.txt
	@./build/skjcc -target riscv -o build/libc-io/fileio tests/libc-io/fileio.c
	@./build/skj-run build/libc-io/fileio build/libc-io/in.txt build/libc-io/out.txt \
	     > build/libc-io/so.skj 2>&1; rc=$$?; cp build/libc-io/out.txt build/libc-io/out.skj 2>/dev/null; \
	 [ $$rc = 0 ] || { echo "FAIL: skj-run exit $$rc"; cat build/libc-io/so.skj; exit 1; }
	@$(CC) -O2 -o build/libc-io/fileio.host tests/libc-io/fileio.c
	@build/libc-io/fileio.host build/libc-io/in.txt build/libc-io/out.txt \
	     > build/libc-io/so.host 2>&1; rc=$$?; cp build/libc-io/out.txt build/libc-io/out.host 2>/dev/null; \
	 [ $$rc = 0 ] || { echo "FAIL: host exit $$rc"; cat build/libc-io/so.host; exit 1; }
	@ok=1; \
	 cmp -s build/libc-io/in.txt build/libc-io/out.skj || { echo "FAIL: skj-run round-trip"; ok=0; }; \
	 cmp -s build/libc-io/in.txt build/libc-io/out.host || { echo "FAIL: host round-trip"; ok=0; }; \
	 cmp -s build/libc-io/so.skj build/libc-io/so.host || { echo "FAIL: skj-run vs host stdout"; ok=0; }; \
	 grep -q "argc=3" build/libc-io/so.skj || { echo "FAIL: argv not delivered"; ok=0; }; \
	 if [ $$ok = 1 ]; then \
	     echo "PASS: libc fopen/fseek/ftell/fread/fwrite + argv (skj-run matches the host)"; \
	 else cat build/libc-io/so.skj build/libc-io/so.host; exit 1; fi

## statx-backed stat/fstat for rv32: rv32 has no stat/fstat syscall, so the libc
## stat family (libc/stat.c) goes through statx (#291); the rv32 crt carries the
## wrapper (runtime/start_rv.S) and skj-run answers statx (emu/guest.c).  A guest
## stat()s and fstat()s a known 12-byte file; skj-run must agree with the same
## program built by the host compiler and run natively, and report the real size
## and type.
test-rv-stat: build/skjcc $(DRIVER_LIB_RV) build/skj-run
	@mkdir -p build/libc-io
	@printf 'hello statx\n' > build/libc-io/stat.txt
	@./build/skjcc -target riscv -o build/libc-io/statio tests/libc-io/statio.c
	@./build/skj-run build/libc-io/statio build/libc-io/stat.txt > build/libc-io/stat.skj 2>&1; \
	 rc=$$?; [ $$rc = 0 ] || { echo "FAIL: skj-run exit $$rc"; cat build/libc-io/stat.skj; exit 1; }
	@$(CC) -O2 -o build/libc-io/statio.host tests/libc-io/statio.c
	@build/libc-io/statio.host build/libc-io/stat.txt > build/libc-io/stat.host 2>&1; \
	 rc=$$?; [ $$rc = 0 ] || { echo "FAIL: host exit $$rc"; cat build/libc-io/stat.host; exit 1; }
	@ok=1; \
	 cmp -s build/libc-io/stat.skj build/libc-io/stat.host || { echo "FAIL: skj-run vs host stdout"; ok=0; }; \
	 grep -q "stat size=12 reg=1 dir=0" build/libc-io/stat.skj || { echo "FAIL: wrong stat output"; ok=0; }; \
	 grep -q "fstat size=12 reg=1" build/libc-io/stat.skj || { echo "FAIL: wrong fstat output"; ok=0; }; \
	 if [ $$ok = 1 ]; then \
	     echo "PASS: libc stat/fstat via statx (skj-run matches the host)"; \
	 else cat build/libc-io/stat.skj build/libc-io/stat.host; exit 1; fi

## The path file operations (mkdir/rename/remove/rmdir) over skj-run's
## GSYS_MKDIRAT/UNLINKAT/RENAMEAT2 handlers, compared against the same program
## built by the host compiler and run natively.  A distinct scratch dir per
## runner keeps the two from colliding.
test-rv-fileops: build/skjcc $(DRIVER_LIB_RV) build/skj-run
	@mkdir -p build/libc-io/pod-skj build/libc-io/pod-host
	@./build/skjcc -target riscv -o build/libc-io/pathops tests/libc-io/pathops.c
	@./build/skj-run build/libc-io/pathops build/libc-io/pod-skj > build/libc-io/pod.skj 2>&1; \
	 rc=$$?; [ $$rc = 0 ] || { echo "FAIL: skj-run exit $$rc"; cat build/libc-io/pod.skj; exit 1; }
	@$(CC) -O2 -o build/libc-io/pathops.host tests/libc-io/pathops.c
	@build/libc-io/pathops.host build/libc-io/pod-host > build/libc-io/pod.host 2>&1; \
	 rc=$$?; [ $$rc = 0 ] || { echo "FAIL: host exit $$rc"; cat build/libc-io/pod.host; exit 1; }
	@ok=1; \
	 cmp -s build/libc-io/pod.skj build/libc-io/pod.host || { echo "FAIL: skj-run vs host stdout"; ok=0; }; \
	 grep -q "mkdir=0"        build/libc-io/pod.skj || { echo "FAIL: mkdir";   ok=0; }; \
	 grep -q "rename=0"       build/libc-io/pod.skj || { echo "FAIL: rename";  ok=0; }; \
	 grep -q "statold=0"      build/libc-io/pod.skj || { echo "FAIL: statold"; ok=0; }; \
	 grep -q "statnew size=3" build/libc-io/pod.skj || { echo "FAIL: statnew"; ok=0; }; \
	 grep -q "remove=0"       build/libc-io/pod.skj || { echo "FAIL: remove";  ok=0; }; \
	 grep -q "rmdir=0"        build/libc-io/pod.skj || { echo "FAIL: rmdir";   ok=0; }; \
	 grep -q "statdir=0"      build/libc-io/pod.skj || { echo "FAIL: statdir"; ok=0; }; \
	 if [ $$ok = 1 ]; then \
	     echo "PASS: libc mkdir/rename/remove/rmdir (skj-run matches the host)"; \
	 else cat build/libc-io/pod.skj build/libc-io/pod.host; exit 1; fi

## Directory reads (opendir/readdir/closedir) over skj-run's GSYS_GETDENTS64
## handler, compared against the same program built by the host compiler and run
## natively.  Each runner gets its own scratch directory of three known files, so
## the readdir order matches (skj-run enumerates through the host's readdir too).
test-rv-dirs: build/skjcc $(DRIVER_LIB_RV) build/skj-run
	@mkdir -p build/libc-io
	@for d in dir-skj dir-host; do \
	    rm -rf build/libc-io/$$d; mkdir -p build/libc-io/$$d; \
	    : > build/libc-io/$$d/alpha; : > build/libc-io/$$d/beta; : > build/libc-io/$$d/gamma; \
	 done
	@./build/skjcc -target riscv -o build/libc-io/dirio tests/libc-io/dirio.c
	@./build/skj-run build/libc-io/dirio build/libc-io/dir-skj > build/libc-io/dir.skj 2>&1; \
	 rc=$$?; [ $$rc = 0 ] || { echo "FAIL: skj-run exit $$rc"; cat build/libc-io/dir.skj; exit 1; }
	@$(CC) -O2 -o build/libc-io/dirio.host tests/libc-io/dirio.c
	@build/libc-io/dirio.host build/libc-io/dir-host > build/libc-io/dir.host 2>&1; \
	 rc=$$?; [ $$rc = 0 ] || { echo "FAIL: host exit $$rc"; cat build/libc-io/dir.host; exit 1; }
	@ok=1; \
	 cmp -s build/libc-io/dir.skj build/libc-io/dir.host || { echo "FAIL: skj-run vs host stdout"; ok=0; }; \
	 grep -q "entries=3" build/libc-io/dir.skj || { echo "FAIL: entry count"; ok=0; }; \
	 grep -q "namelen=14" build/libc-io/dir.skj || { echo "FAIL: name lengths"; ok=0; }; \
	 if [ $$ok = 1 ]; then \
	     echo "PASS: libc opendir/readdir (skj-run matches the host)"; \
	 else cat build/libc-io/dir.skj build/libc-io/dir.host; exit 1; fi

## The ColdFire siblings of test-rv-stat / -fileops / -dirs: the same libc file-op
## fixtures compiled -target coldfire and run under skj-run, whose CFV4e core and
## m68k syscall maps (emu/cf_user.c) answer statx / mkdirat / unlinkat / renameat2
## / getdents64, compared against the same program built by the host compiler and
## run natively.  This exercises the emulator's ColdFire file-metadata handlers
## the way the rv targets exercise the rv ones; check-libc-cf already runs the
## fixtures under qemu alone.
test-cf-stat: build/skjcc $(DRIVER_LIB_CF) build/skj-run
	@mkdir -p build/libc-io-cf
	@printf 'hello statx\n' > build/libc-io-cf/stat.txt
	@./build/skjcc -target coldfire -o build/libc-io-cf/statio tests/libc-io/statio.c
	@./build/skj-run build/libc-io-cf/statio build/libc-io-cf/stat.txt > build/libc-io-cf/stat.skj 2>&1; \
	 rc=$$?; [ $$rc = 0 ] || { echo "FAIL: skj-run exit $$rc"; cat build/libc-io-cf/stat.skj; exit 1; }
	@$(CC) -O2 -o build/libc-io-cf/statio.host tests/libc-io/statio.c
	@build/libc-io-cf/statio.host build/libc-io-cf/stat.txt > build/libc-io-cf/stat.host 2>&1; \
	 rc=$$?; [ $$rc = 0 ] || { echo "FAIL: host exit $$rc"; cat build/libc-io-cf/stat.host; exit 1; }
	@ok=1; \
	 cmp -s build/libc-io-cf/stat.skj build/libc-io-cf/stat.host || { echo "FAIL: skj-run vs host stdout"; ok=0; }; \
	 grep -q "stat size=12 reg=1 dir=0" build/libc-io-cf/stat.skj || { echo "FAIL: wrong stat output"; ok=0; }; \
	 grep -q "fstat size=12 reg=1" build/libc-io-cf/stat.skj || { echo "FAIL: wrong fstat output"; ok=0; }; \
	 if [ $$ok = 1 ]; then \
	     echo "PASS: libc stat/fstat via statx on ColdFire (skj-run matches the host)"; \
	 else cat build/libc-io-cf/stat.skj build/libc-io-cf/stat.host; exit 1; fi

test-cf-fileops: build/skjcc $(DRIVER_LIB_CF) build/skj-run
	@mkdir -p build/libc-io-cf/pod-skj build/libc-io-cf/pod-host
	@./build/skjcc -target coldfire -o build/libc-io-cf/pathops tests/libc-io/pathops.c
	@./build/skj-run build/libc-io-cf/pathops build/libc-io-cf/pod-skj > build/libc-io-cf/pod.skj 2>&1; \
	 rc=$$?; [ $$rc = 0 ] || { echo "FAIL: skj-run exit $$rc"; cat build/libc-io-cf/pod.skj; exit 1; }
	@$(CC) -O2 -o build/libc-io-cf/pathops.host tests/libc-io/pathops.c
	@build/libc-io-cf/pathops.host build/libc-io-cf/pod-host > build/libc-io-cf/pod.host 2>&1; \
	 rc=$$?; [ $$rc = 0 ] || { echo "FAIL: host exit $$rc"; cat build/libc-io-cf/pod.host; exit 1; }
	@ok=1; \
	 cmp -s build/libc-io-cf/pod.skj build/libc-io-cf/pod.host || { echo "FAIL: skj-run vs host stdout"; ok=0; }; \
	 grep -q "mkdir=0"        build/libc-io-cf/pod.skj || { echo "FAIL: mkdir";   ok=0; }; \
	 grep -q "rename=0"       build/libc-io-cf/pod.skj || { echo "FAIL: rename";  ok=0; }; \
	 grep -q "statold=0"      build/libc-io-cf/pod.skj || { echo "FAIL: statold"; ok=0; }; \
	 grep -q "statnew size=3" build/libc-io-cf/pod.skj || { echo "FAIL: statnew"; ok=0; }; \
	 grep -q "remove=0"       build/libc-io-cf/pod.skj || { echo "FAIL: remove";  ok=0; }; \
	 grep -q "rmdir=0"        build/libc-io-cf/pod.skj || { echo "FAIL: rmdir";   ok=0; }; \
	 grep -q "statdir=0"      build/libc-io-cf/pod.skj || { echo "FAIL: statdir"; ok=0; }; \
	 if [ $$ok = 1 ]; then \
	     echo "PASS: libc mkdir/rename/remove/rmdir on ColdFire (skj-run matches the host)"; \
	 else cat build/libc-io-cf/pod.skj build/libc-io-cf/pod.host; exit 1; fi

test-cf-dirs: build/skjcc $(DRIVER_LIB_CF) build/skj-run
	@mkdir -p build/libc-io-cf
	@for d in dir-skj dir-host; do \
	    rm -rf build/libc-io-cf/$$d; mkdir -p build/libc-io-cf/$$d; \
	    : > build/libc-io-cf/$$d/alpha; : > build/libc-io-cf/$$d/beta; : > build/libc-io-cf/$$d/gamma; \
	 done
	@./build/skjcc -target coldfire -o build/libc-io-cf/dirio tests/libc-io/dirio.c
	@./build/skj-run build/libc-io-cf/dirio build/libc-io-cf/dir-skj > build/libc-io-cf/dir.skj 2>&1; \
	 rc=$$?; [ $$rc = 0 ] || { echo "FAIL: skj-run exit $$rc"; cat build/libc-io-cf/dir.skj; exit 1; }
	@$(CC) -O2 -o build/libc-io-cf/dirio.host tests/libc-io/dirio.c
	@build/libc-io-cf/dirio.host build/libc-io-cf/dir-host > build/libc-io-cf/dir.host 2>&1; \
	 rc=$$?; [ $$rc = 0 ] || { echo "FAIL: host exit $$rc"; cat build/libc-io-cf/dir.host; exit 1; }
	@ok=1; \
	 cmp -s build/libc-io-cf/dir.skj build/libc-io-cf/dir.host || { echo "FAIL: skj-run vs host stdout"; ok=0; }; \
	 grep -q "entries=3" build/libc-io-cf/dir.skj || { echo "FAIL: entry count"; ok=0; }; \
	 grep -q "namelen=14" build/libc-io-cf/dir.skj || { echo "FAIL: name lengths"; ok=0; }; \
	 if [ $$ok = 1 ]; then \
	     echo "PASS: libc opendir/readdir on ColdFire (skj-run matches the host)"; \
	 else cat build/libc-io-cf/dir.skj build/libc-io-cf/dir.host; exit 1; fi

## The confined-capability mount end to end: a C guest built with the
## fs/-interpose libc (FS_INTERPOSE) is run under skj-run with --mount, so the
## launcher opens the granted directory as a capability fd and fills the guest
## fs_mounts table.  Paths through the "data" mount reach the confined hostcap
## backend (openat against the capability fd); a seeded file reads back, a
## written file round-trips, and a "data:../escape.txt" open is denied although
## the escape target exists just outside the granted directory.  This is the one
## place the launcher --mount path and the guest-side confined backend meet, so
## it cannot run under bare qemu (no sandbox fd table / mount installer).  The
## guest self-checks and exits 42; run-libc-tests.sh drives it with skj-run as
## QEMU.  rv and cf cover both freestanding VM tiers.
## The skj-run binary the fs-cap confinement tests drive.  check-emu-posix
## overrides it with build/skj-run-posix (-DHP_FORCE_POSIX) so the same end-to-end
## confinement runs through cap_walk on Linux as well as the openat2 default.
FS_CAP_RUN ?= build/skj-run

test-rv-fs-cap: build/skj-cc-rv build/rv/start.o build/rv/va_rv.o build/rv/soft64_div.o build/rv/co_switch.o $(FS_CAP_RUN)
	@rm -rf build/fs-cap-rv
	@mkdir -p build/fs-cap-rv/data
	@printf 'capability-granted\n' > build/fs-cap-rv/data/seed.txt
	@printf 'ESCAPED\n' > build/fs-cap-rv/escape.txt
	@CCBIN="$(CURDIR)/build/skj-cc-rv" \
	 ASM="$(RV_AS) -march=rv32imfd_zfh -mabi=ilp32 -o" \
	 LD="$(RV_LD) -m elf32lriscv -o" \
	 QEMU="$(CURDIR)/$(FS_CAP_RUN) --mount data:$(CURDIR)/build/fs-cap-rv/data" \
	 START="$(CURDIR)/build/rv/start.o" \
	 HALF="$(CURDIR)/build/rv/va_rv.o $(CURDIR)/build/rv/soft64_div.o $(CURDIR)/build/rv/co_switch.o" \
	 OUTDIR="$(CURDIR)/build/fs-cap-rv" \
	 TEST="$(CURDIR)/tests/fs-cap/capguest.c" \
	 FS_INTERPOSE=1 EXPECT=42 \
	 sh tests/run-libc-tests.sh

test-cf-fs-cap: build/skj-cc build/skj-as build/start.o build/half.o build/va_cf.o build/co_switch_cf.o $(FS_CAP_RUN)
	@rm -rf build/fs-cap-cf
	@mkdir -p build/fs-cap-cf/data
	@printf 'capability-granted\n' > build/fs-cap-cf/data/seed.txt
	@printf 'ESCAPED\n' > build/fs-cap-cf/escape.txt
	@OUTDIR="$(CURDIR)/build/fs-cap-cf" \
	 QEMU="$(CURDIR)/$(FS_CAP_RUN) --mount data:$(CURDIR)/build/fs-cap-cf/data" \
	 HALF="$(CURDIR)/build/half.o $(CURDIR)/build/va_cf.o $(CURDIR)/build/co_switch_cf.o" \
	 TEST="$(CURDIR)/tests/fs-cap/capguest.c" \
	 FS_INTERPOSE=1 EXPECT=42 \
	 sh tests/run-libc-tests.sh

## Run the fs-cap confinement end to end against build/skj-run-posix, so the
## cap_walk component walk (the portable confinement floor every non-Linux host
## uses) is compiled and exercised on Linux too, where openat2 is the default.
## A recursive make re-drives the fs-cap targets with the forced-POSIX runner.
check-emu-posix: build/skj-run-posix
	@$(MAKE) --no-print-directory test-rv-fs-cap test-cf-fs-cap FS_CAP_RUN=build/skj-run-posix
	@echo "PASS: fs-cap confinement via cap_walk (-DHP_FORCE_POSIX) on $$(uname -s)"

## The C guest's RNG: libc rand/srand draw from the shared fast generator and
## arc4random* from the shared ChaCha20 CSPRNG (runtime/rng.c), the same code
## Kobold's std.random uses.  rand/srand are deterministic and covered on every
## arch by check-libc (phase F); arc4random seeds from the getrandom seam, which
## only the rv/cf crts carry, so it is exercised here on the two freestanding
## tiers.  The output is entropy-seeded, so the self-check (exit 42: a
## non-degenerate stream, in-range uniform draws, every histogram bucket hit) is
## run under both skj-run (the emulator's GSYS_GETRANDOM, boris's path) and the
## real kernel under qemu, not compared byte for byte.
test-rv-arc4random: build/skjcc $(DRIVER_LIB_RV) build/skj-run
	@mkdir -p build/libc-io
	@./build/skjcc -target riscv -o build/libc-io/a4r tests/libc_arc4random.c
	@./build/skj-run build/libc-io/a4r > build/libc-io/a4r.skj 2>&1; \
	 rc=$$?; [ $$rc = 42 ] || { echo "FAIL: skj-run exit $$rc (want 42)"; cat build/libc-io/a4r.skj; exit 1; }
	@$(QEMU_RV) build/libc-io/a4r > build/libc-io/a4r.qemu 2>&1; \
	 rc=$$?; [ $$rc = 42 ] || { echo "FAIL: qemu exit $$rc (want 42)"; cat build/libc-io/a4r.qemu; exit 1; }
	@echo "PASS: libc arc4random/_buf/_uniform over the shared CSPRNG (skj-run and qemu)"

test-cf-arc4random: build/skjcc $(DRIVER_LIB_CF) build/skj-run
	@mkdir -p build/libc-io-cf
	@./build/skjcc -target coldfire -o build/libc-io-cf/a4r tests/libc_arc4random.c
	@./build/skj-run build/libc-io-cf/a4r > build/libc-io-cf/a4r.skj 2>&1; \
	 rc=$$?; [ $$rc = 42 ] || { echo "FAIL: skj-run exit $$rc (want 42)"; cat build/libc-io-cf/a4r.skj; exit 1; }
	@$(QEMU) build/libc-io-cf/a4r > build/libc-io-cf/a4r.qemu 2>&1; \
	 rc=$$?; [ $$rc = 42 ] || { echo "FAIL: qemu exit $$rc (want 42)"; cat build/libc-io-cf/a4r.qemu; exit 1; }
	@echo "PASS: libc arc4random/_buf/_uniform over the shared CSPRNG on ColdFire (skj-run and qemu)"

## Self-host: build skj-cc itself as an rv32 guest (its own sources compiled by
## skj-cc-rv and linked against the rv libc), then run it under skj-run to
## compile a small C program entirely through the sandbox syscalls.  The emitted
## assembly must match a qemu run of the same in-guest compiler, and assembling,
## linking and running it must yield the program's own result (add(40,2) == 42).
## This exercises the full file-I/O surface (open/read/write) plus setjmp, time,
## getenv and the 64-bit strto* the compiler needs.
test-rv-selfhost: build/skjcc build/skj-cc-rv build/skj-as-rv build/skj-ld-rv \
                  $(DRIVER_LIB_RV) build/skj-run tests/selfhost/add.c
	@mkdir -p build/selfhost
	@set -e; objs=""; for s in $(CC_SRC_RV); do \
	    b=$$(basename $$s .c); \
	    ./build/skjcc -target riscv -DCC_RISCV -c -Ibuild -Icc -Icpp -Iir -Ibackend \
	        $$s -o build/selfhost/$$b.o; \
	    objs="$$objs build/selfhost/$$b.o"; \
	 done; \
	 ./build/skjcc -target riscv $$objs -o build/selfhost/skj-cc-rv.elf
	@./build/skj-run build/selfhost/skj-cc-rv.elf -o build/selfhost/add.skj.s \
	    tests/selfhost/add.c 2>build/selfhost/skj.err \
	    || { echo "FAIL: skj-run of in-guest skj-cc"; cat build/selfhost/skj.err; exit 1; }
	@$(QEMU_RV) build/selfhost/skj-cc-rv.elf -o build/selfhost/add.qemu.s \
	    tests/selfhost/add.c 2>build/selfhost/qemu.err \
	    || { echo "FAIL: qemu of in-guest skj-cc"; cat build/selfhost/qemu.err; exit 1; }
	@cmp -s build/selfhost/add.skj.s build/selfhost/add.qemu.s \
	    || { echo "FAIL: in-guest asm differs skj-run vs qemu"; exit 1; }
	@./build/skj-as-rv -o build/selfhost/add.o build/selfhost/add.skj.s
	@./build/skj-ld-rv -o build/selfhost/add.elf build/lib/riscv/crt0.o \
	    build/selfhost/add.o build/lib/riscv/libc.a
	@./build/skj-run build/selfhost/add.elf; rc=$$?; \
	 [ $$rc = 42 ] || { echo "FAIL: in-guest-compiled program exit $$rc (want 42)"; exit 1; }
	@echo "PASS: skj-cc self-hosts under skj-run (compiled add.c in-guest -> 42)"

## Full translation unit through the sandbox: the in-guest skj-cc (reused from
## test-rv-selfhost) compiles the compiler's own largest source, cc/lower.c
## (~4900 lines), under skj-run.  add.c was a toy; this is a real TU with the
## full preprocessor reading many include files and the biggest functions in
## the tree, so it exercises the sandbox heap (brk-backed sbrk) and the whole
## compile path at scale.  The emitted asm must match a qemu run of the same
## in-guest compiler byte-for-byte, and skj-as-rv must assemble it.
test-rv-selfhost-full: test-rv-selfhost build/skj-as-rv
	@mkdir -p build/selfhost-full
	@FLAGS="-DCC_RISCV -Iinclude -Ibuild -Icc -Icpp -Iir -Ibackend"; \
	 ./build/skj-run build/selfhost/skj-cc-rv.elf $$FLAGS \
	     -o build/selfhost-full/lower.skj.s cc/lower.c 2>build/selfhost-full/skj.err \
	     || { echo "FAIL: skj-run full-TU compile"; cat build/selfhost-full/skj.err; exit 1; }; \
	 $(QEMU_RV) build/selfhost/skj-cc-rv.elf $$FLAGS \
	     -o build/selfhost-full/lower.qemu.s cc/lower.c 2>build/selfhost-full/qemu.err \
	     || { echo "FAIL: qemu full-TU compile"; cat build/selfhost-full/qemu.err; exit 1; }; \
	 cmp -s build/selfhost-full/lower.skj.s build/selfhost-full/lower.qemu.s \
	     || { echo "FAIL: full-TU asm differs skj-run vs qemu"; exit 1; }; \
	 ./build/skj-as-rv -o build/selfhost-full/lower.o build/selfhost-full/lower.skj.s \
	     || { echo "FAIL: skj-as-rv rejected the full-TU asm"; exit 1; }
	@echo "PASS: skj-cc translates a full TU (cc/lower.c) in-guest under skj-run == qemu, asm assembles"

## The ColdFire counterpart of test-rv-selfhost: skj-cc built as a ColdFire
## guest (its own sources via skj-cc + skj-as + skj-ld, linked against the cf
## libc), run under skj-run to compile add.c through the sandbox syscalls, its
## asm checked against qemu-m68k, then assembled, linked and run for exit 42.
## This exercises emit_insn past 32 KiB (the skj-as long-branch form) and the
## ColdFire crt's argv/environ setup.
test-cf-selfhost: build/skjcc build/skj-cc build/skj-as build/skj-ld \
                  $(DRIVER_LIB_CF) build/skj-run tests/selfhost/add.c
	@mkdir -p build/selfhost-cf
	@set -e; objs=""; for s in $(CC_SRC); do \
	    b=$$(basename $$s .c); \
	    ./build/skjcc -target coldfire -DCC_M68K -c -Ibuild -Icc -Icpp -Iir -Ibackend \
	        $$s -o build/selfhost-cf/$$b.o; \
	    objs="$$objs build/selfhost-cf/$$b.o"; \
	 done; \
	 ./build/skjcc -target coldfire $$objs -o build/selfhost-cf/skj-cc-cf.elf
	@./build/skj-run build/selfhost-cf/skj-cc-cf.elf -o build/selfhost-cf/add.skj.s \
	    tests/selfhost/add.c 2>build/selfhost-cf/skj.err \
	    || { echo "FAIL: skj-run of in-guest skj-cc"; cat build/selfhost-cf/skj.err; exit 1; }
	@$(QEMU) build/selfhost-cf/skj-cc-cf.elf -o build/selfhost-cf/add.qemu.s \
	    tests/selfhost/add.c 2>build/selfhost-cf/qemu.err \
	    || { echo "FAIL: qemu of in-guest skj-cc"; cat build/selfhost-cf/qemu.err; exit 1; }
	@cmp -s build/selfhost-cf/add.skj.s build/selfhost-cf/add.qemu.s \
	    || { echo "FAIL: in-guest asm differs skj-run vs qemu"; exit 1; }
	@./build/skj-as -o build/selfhost-cf/add.o build/selfhost-cf/add.skj.s
	@./build/skj-ld -o build/selfhost-cf/add.elf build/lib/coldfire/crt0.o \
	    build/selfhost-cf/add.o build/lib/coldfire/libc.a
	@./build/skj-run build/selfhost-cf/add.elf; rc=$$?; \
	 [ $$rc = 42 ] || { echo "FAIL: in-guest-compiled program exit $$rc (want 42)"; exit 1; }
	@echo "PASS: skj-cc self-hosts under skj-run (compiled add.c in-guest -> 42, ColdFire)"

## Full translation unit through the ColdFire sandbox, the counterpart of
## test-rv-selfhost-full: the in-guest skj-cc (reused from test-cf-selfhost)
## compiles the compiler's own largest source, cc/lower.c (~4900 lines), under
## skj-run.  add.c was a toy; this is a real TU with the full preprocessor
## reading many include files and the biggest functions in the tree, so it
## exercises the ColdFire sandbox heap and the whole compile path at scale.  The
## emitted asm must match a qemu-m68k run of the same in-guest compiler
## byte-for-byte, and skj-as (the long-branch form past 32 KiB) must assemble it.
test-cf-selfhost-full: test-cf-selfhost build/skj-as
	@mkdir -p build/selfhost-cf-full
	@FLAGS="-DCC_M68K -Iinclude -Ibuild -Icc -Icpp -Iir -Ibackend"; \
	 ./build/skj-run build/selfhost-cf/skj-cc-cf.elf $$FLAGS \
	     -o build/selfhost-cf-full/lower.skj.s cc/lower.c 2>build/selfhost-cf-full/skj.err \
	     || { echo "FAIL: skj-run full-TU compile"; cat build/selfhost-cf-full/skj.err; exit 1; }; \
	 $(QEMU) build/selfhost-cf/skj-cc-cf.elf $$FLAGS \
	     -o build/selfhost-cf-full/lower.qemu.s cc/lower.c 2>build/selfhost-cf-full/qemu.err \
	     || { echo "FAIL: qemu full-TU compile"; cat build/selfhost-cf-full/qemu.err; exit 1; }; \
	 cmp -s build/selfhost-cf-full/lower.skj.s build/selfhost-cf-full/lower.qemu.s \
	     || { echo "FAIL: full-TU asm differs skj-run vs qemu"; exit 1; }; \
	 ./build/skj-as -o build/selfhost-cf-full/lower.o build/selfhost-cf-full/lower.skj.s \
	     || { echo "FAIL: skj-as rejected the full-TU asm"; exit 1; }
	@echo "PASS: skj-cc translates a full TU (cc/lower.c) in-guest under skj-run == qemu, asm assembles (ColdFire)"

## ---------------------------------------------------------------------------
## Whole-toolchain self-host: skj-as and skj-ld as guest ELFs (RV32 + ColdFire)
##
## test-*-selfhost above builds skj-cc as a guest ELF but still runs skj-as and
## skj-ld on the host.  These build the assembler and linker as guest ELFs too
## (their own sources compiled by skjcc against the guest libc), so the full
## cc -> as -> ld pipeline runs under skj-run.  skj-ld links in-guest because
## ld/mapfile.c has a __skj__ branch that reads an object into a malloc buffer
## instead of mmap (the guest libc has no sys/mman.h).

build/selfhost/skj-as-rv.elf: $(AS_RV_SRC) build/skjcc $(DRIVER_LIB_RV) build/version.h | build
	@mkdir -p build/selfhost
	@set -e; objs=""; for s in $(AS_RV_SRC); do \
	    b=$$(basename $$s .c); \
	    ./build/skjcc -target riscv -c -Ibuild -Ias -Iir -Idwarf $$s -o build/selfhost/asrv_$$b.o; \
	    objs="$$objs build/selfhost/asrv_$$b.o"; \
	 done; \
	 ./build/skjcc -target riscv $$objs -o $@

build/selfhost/skj-ld-rv.elf: $(LD_RV_SRC) ld/mapfile.c build/skjcc $(DRIVER_LIB_RV) build/version.h | build
	@mkdir -p build/selfhost
	@set -e; objs=""; for s in $(LD_RV_SRC); do \
	    b=$$(basename $$s .c); \
	    ./build/skjcc -target riscv -c -Ibuild -Ild -Iir $$s -o build/selfhost/ldrv_$$b.o; \
	    objs="$$objs build/selfhost/ldrv_$$b.o"; \
	 done; \
	 ./build/skjcc -target riscv $$objs -o $@

build/selfhost-cf/skj-as-cf.elf: $(AS_SRC) build/skjcc $(DRIVER_LIB_CF) build/version.h | build
	@mkdir -p build/selfhost-cf
	@set -e; objs=""; for s in $(AS_SRC); do \
	    b=$$(basename $$s .c); \
	    ./build/skjcc -target coldfire -c -Ibuild -Ias -Iir -Idwarf $$s -o build/selfhost-cf/ascf_$$b.o; \
	    objs="$$objs build/selfhost-cf/ascf_$$b.o"; \
	 done; \
	 ./build/skjcc -target coldfire $$objs -o $@

build/selfhost-cf/skj-ld-cf.elf: $(LD_SRC) ld/mapfile.c build/skjcc $(DRIVER_LIB_CF) build/version.h | build
	@mkdir -p build/selfhost-cf
	@set -e; objs=""; for s in $(LD_SRC); do \
	    b=$$(basename $$s .c); \
	    ./build/skjcc -target coldfire -c -Ibuild -Ild -Iir $$s -o build/selfhost-cf/ldcf_$$b.o; \
	    objs="$$objs build/selfhost-cf/ldcf_$$b.o"; \
	 done; \
	 ./build/skjcc -target coldfire $$objs -o $@

## test-rv-selfhost-toolchain: assemble and link the in-guest-compiled add.c
## with the in-guest skj-as-rv and skj-ld-rv too, then run the result (-> 42).
## The whole C toolchain runs inside the sandbox.
test-rv-selfhost-toolchain: test-rv-selfhost build/selfhost/skj-as-rv.elf \
                            build/selfhost/skj-ld-rv.elf
	@./build/skj-run build/selfhost/skj-as-rv.elf -o build/selfhost/add.guest.o \
	    build/selfhost/add.skj.s 2>build/selfhost/as.err \
	    || { echo "FAIL: in-guest skj-as-rv"; cat build/selfhost/as.err; exit 1; }
	@./build/skj-run build/selfhost/skj-ld-rv.elf -o build/selfhost/add.guest.elf \
	    build/lib/riscv/crt0.o build/selfhost/add.guest.o build/lib/riscv/libc.a \
	    2>build/selfhost/ld.err \
	    || { echo "FAIL: in-guest skj-ld-rv"; cat build/selfhost/ld.err; exit 1; }
	@./build/skj-run build/selfhost/add.guest.elf; rc=$$?; \
	 [ $$rc = 42 ] || { echo "FAIL: cc+as+ld in-guest program exit $$rc (want 42)"; exit 1; }
	@echo "PASS: skj cc+as+ld all self-host under skj-run (RV32) -> 42"

test-cf-selfhost-toolchain: test-cf-selfhost build/selfhost-cf/skj-as-cf.elf \
                            build/selfhost-cf/skj-ld-cf.elf
	@./build/skj-run build/selfhost-cf/skj-as-cf.elf -o build/selfhost-cf/add.guest.o \
	    build/selfhost-cf/add.skj.s 2>build/selfhost-cf/as.err \
	    || { echo "FAIL: in-guest skj-as-cf"; cat build/selfhost-cf/as.err; exit 1; }
	@./build/skj-run build/selfhost-cf/skj-ld-cf.elf -o build/selfhost-cf/add.guest.elf \
	    build/lib/coldfire/crt0.o build/selfhost-cf/add.guest.o build/lib/coldfire/libc.a \
	    2>build/selfhost-cf/ld.err \
	    || { echo "FAIL: in-guest skj-ld-cf"; cat build/selfhost-cf/ld.err; exit 1; }
	@./build/skj-run build/selfhost-cf/add.guest.elf; rc=$$?; \
	 [ $$rc = 42 ] || { echo "FAIL: cc+as+ld in-guest program exit $$rc (want 42)"; exit 1; }
	@echo "PASS: skj cc+as+ld all self-host under skj-run (ColdFire) -> 42"

## ---------------------------------------------------------------------------
## Kobold compiler (skj-kpc) self-host: a freestanding kpcom guest ELF, linked
## entirely by the skj toolchain (skj-kp -> skj-as -> skj-ld + the freestanding
## runtime), run under skj-run to compile Kobold.  No file-I/O code is needed:
## kobold/io_free.c already supplies ParamStr/ParamCount, ReadFileStr/
## WriteFileStr and the kpd_* directory surface over open/read/write/lseek.

build/selfhost/skj-kpc-rv.elf: build/kpcom-rv.kpm build/skj-kp-rv build/skj-as-rv \
                               build/skj-ld-rv build/kpfree-rv/kp_crt_rv.o \
                               build/libkpaot-rv-free.a | build
	@mkdir -p build/selfhost
	./build/skj-kp-rv build/kpcom-rv.kpm -o build/selfhost/kpcom-free-rv.s
	./build/skj-as-rv -o build/selfhost/kpcom-free-rv.o build/selfhost/kpcom-free-rv.s
	./build/skj-ld-rv -o $@ build/kpfree-rv/kp_crt_rv.o \
	    build/selfhost/kpcom-free-rv.o build/libkpaot-rv-free.a

build/selfhost-cf/skj-kpc-cf.elf: build/kpcom-cf.kpm build/skj-kp-cf build/skj-as \
                                  build/skj-ld build/kpfree-cf/kp_crt_cf.o \
                                  build/libkpaot-cf-free.a | build
	@mkdir -p build/selfhost-cf
	./build/skj-kp-cf build/kpcom-cf.kpm -o build/selfhost-cf/kpcom-free-cf.s
	./build/skj-as -o build/selfhost-cf/kpcom-free-cf.o build/selfhost-cf/kpcom-free-cf.s
	./build/skj-ld -o $@ build/kpfree-cf/kp_crt_cf.o \
	    build/selfhost-cf/kpcom-free-cf.o build/libkpaot-cf-free.a

## test-{rv,cf}-selfhost-kpc: run the freestanding skj-kpc guest ELF under
## skj-run to --emit-kpm a small unit-free program (hello.pas) and assert the
## emitted KPM1 is byte-identical to the host skj-kpc output.  This proves the
## Kobold compiler compiles in-guest over the guest file I/O (ParamStr, the
## source read, the KPM write).  The far heavier self-compile of kpcom's own
## 480 KB source is the opt-in -full target below (minutes under the RV32
## interpreter), kept out of the routine suite.
test-rv-selfhost-kpc: build/selfhost/skj-kpc-rv.elf build/skj-kpc-rv build/skj-run \
                      tests/kp-corpus/hello.pas
	@mkdir -p build/selfhost
	@./build/skj-kpc-rv --emit-kpm tests/kp-corpus/hello.pas > build/selfhost/hello.host.kpm
	@./build/skj-run build/selfhost/skj-kpc-rv.elf --emit-kpm tests/kp-corpus/hello.pas \
	    > build/selfhost/hello.guest.kpm 2>build/selfhost/kpc.err \
	    || { echo "FAIL: in-guest skj-kpc (hello)"; cat build/selfhost/kpc.err; exit 1; }
	@cmp -s build/selfhost/hello.host.kpm build/selfhost/hello.guest.kpm \
	    || { echo "FAIL: in-guest KPM differs from host (hello)"; exit 1; }
	@echo "PASS: skj-kpc self-hosts under skj-run (RV32): hello KPM == host"

test-cf-selfhost-kpc: build/selfhost-cf/skj-kpc-cf.elf build/skj-kpc-cf build/skj-run \
                      tests/kp-corpus/hello.pas
	@mkdir -p build/selfhost-cf
	@./build/skj-kpc-cf --emit-kpm tests/kp-corpus/hello.pas > build/selfhost-cf/hello.host.kpm
	@./build/skj-run build/selfhost-cf/skj-kpc-cf.elf --emit-kpm tests/kp-corpus/hello.pas \
	    > build/selfhost-cf/hello.guest.kpm 2>build/selfhost-cf/kpc.err \
	    || { echo "FAIL: in-guest skj-kpc (hello)"; cat build/selfhost-cf/kpc.err; exit 1; }
	@cmp -s build/selfhost-cf/hello.host.kpm build/selfhost-cf/hello.guest.kpm \
	    || { echo "FAIL: in-guest KPM differs from host (hello)"; exit 1; }
	@echo "PASS: skj-kpc self-hosts under skj-run (ColdFire): hello KPM == host"

## The full self-compile: the in-guest skj-kpc compiles kpcom's own source and
## the emitted KPM1 must match the host.  Opt-in and slow (the 480 KB source
## through the RV32/ColdFire interpreter takes minutes), and it needs a larger
## guest heap than the 16M default, so --heap 256M.
test-rv-selfhost-kpc-full: build/selfhost/skj-kpc-rv.elf build/skj-kpc-rv build/skj-run \
                           kobold/kpcom.pas
	@mkdir -p build/selfhost
	@./build/skj-kpc-rv --emit-kpm kobold/kpcom.pas > build/selfhost/kpcom.host.kpm
	@./build/skj-run --heap 256M build/selfhost/skj-kpc-rv.elf --emit-kpm kobold/kpcom.pas \
	    > build/selfhost/kpcom.guest.kpm 2>build/selfhost/kpc2.err \
	    || { echo "FAIL: in-guest skj-kpc (kpcom self-compile)"; cat build/selfhost/kpc2.err; exit 1; }
	@cmp -s build/selfhost/kpcom.host.kpm build/selfhost/kpcom.guest.kpm \
	    || { echo "FAIL: in-guest kpcom KPM differs from host"; exit 1; }
	@echo "PASS: skj-kpc compiles its own source in-guest under skj-run (RV32): KPM == host"

test-cf-selfhost-kpc-full: build/selfhost-cf/skj-kpc-cf.elf build/skj-kpc-cf build/skj-run \
                           kobold/kpcom.pas
	@mkdir -p build/selfhost-cf
	@./build/skj-kpc-cf --emit-kpm kobold/kpcom.pas > build/selfhost-cf/kpcom.host.kpm
	@./build/skj-run --heap 256M build/selfhost-cf/skj-kpc-cf.elf --emit-kpm kobold/kpcom.pas \
	    > build/selfhost-cf/kpcom.guest.kpm 2>build/selfhost-cf/kpc2.err \
	    || { echo "FAIL: in-guest skj-kpc (kpcom self-compile)"; cat build/selfhost-cf/kpc2.err; exit 1; }
	@cmp -s build/selfhost-cf/kpcom.host.kpm build/selfhost-cf/kpcom.guest.kpm \
	    || { echo "FAIL: in-guest kpcom KPM differs from host"; exit 1; }
	@echo "PASS: skj-kpc compiles its own source in-guest under skj-run (ColdFire): KPM == host"

## Umbrella: the full in-guest self-host suite (opt-in, not part of make check).
## The slow kpcom self-compile (-full targets) is left out; run it on demand.
check-selfhost-all: test-rv-selfhost-toolchain test-cf-selfhost-toolchain \
                    test-rv-selfhost-kpc test-cf-selfhost-kpc
	@echo "PASS: skj toolchain (cc+as+ld+kpc) self-hosts under skj-run (RV32 + ColdFire)"

## The heavy self-compiles, run under the skj-run interpreter: a full C TU
## (cc/lower.c) and kpcom's own ~480 KB source compiled in-guest, minutes each.
## The light hello.pas / add.c self-hosts in check-all already prove the
## mechanism, so these deeper invariants run on their own tier rather than
## gating every check-all.  Run before a release or on demand.
check-slow: test-rv-selfhost-full test-cf-selfhost-full \
            test-rv-selfhost-kpc-full test-cf-selfhost-kpc-full
	@echo "PASS: full in-guest self-compiles (cc TU + kpcom) on RV32 + ColdFire"

## The MIPS counterpart of the self-host tests.  skj-run decodes only ColdFire
## and RV32 ELFs, so the in-guest compiler runs under qemu-mipsel rather than
## the sandbox: build skj-cc-mips from its own sources (skj-cc-mips + skj-as-mips
## + skj-ld-mips, linked against the mips libc), run it under qemu-mipsel to
## compile add.c, then assemble, link and run the result for exit 42.  This is
## the one self-host path that pulls open_memstream into the guest libc, since
## mips_emit.c references it (the .set noreorder scheduler's buffer).
test-mips-selfhost: build/skjcc build/skj-cc-mips build/skj-as-mips build/skj-ld-mips \
                    $(DRIVER_LIB_MIPS) tests/selfhost/add.c
	@mkdir -p build/selfhost-mips
	@set -e; objs=""; for s in $(CC_SRC_MIPS); do \
	    b=$$(basename $$s .c); \
	    ./build/skjcc -target mips -DCC_MIPS -c -Ibuild -Icc -Icpp -Iir -Ibackend \
	        $$s -o build/selfhost-mips/$$b.o; \
	    objs="$$objs build/selfhost-mips/$$b.o"; \
	 done; \
	 ./build/skjcc -target mips $$objs -o build/selfhost-mips/skj-cc-mips.elf
	@$(QEMU_MIPS) build/selfhost-mips/skj-cc-mips.elf -o build/selfhost-mips/add.s \
	    tests/selfhost/add.c 2>build/selfhost-mips/err \
	    || { echo "FAIL: qemu of in-guest skj-cc"; cat build/selfhost-mips/err; exit 1; }
	@./build/skj-as-mips -o build/selfhost-mips/add.o build/selfhost-mips/add.s
	@./build/skj-ld-mips -o build/selfhost-mips/add.elf build/lib/mips/crt0.o \
	    build/selfhost-mips/add.o build/lib/mips/libc.a
	@$(QEMU_MIPS) build/selfhost-mips/add.elf; rc=$$?; \
	 [ $$rc = 42 ] || { echo "FAIL: in-guest-compiled program exit $$rc (want 42)"; exit 1; }
	@echo "PASS: skj-cc self-hosts under qemu-mipsel (compiled add.c in-guest -> 42, MIPS)"

## I64 integration test (IR builder → arm64 asm → qemu-aarch64)
I64_TEST_A64_SRC := tests/test_i64.c $(IR_SRC) $(BE_A64)

build/test_i64_arm64: $(I64_TEST_A64_SRC) ir/ir.h | build
	$(CC) $(CFLAGS) -DSKJ_LP64=1 -Iir -o $@ $(I64_TEST_A64_SRC)

build/arm64/i64_test.s: build/test_i64_arm64 | build/arm64
	./build/test_i64_arm64 -o $@

build/arm64/i64_test.o: build/arm64/i64_test.s $(A64_ASM_DEP)
	$(A64_AS) -o $@ $<

build/arm64/i64_test: build/arm64/i64_test.o build/arm64/start.o
	$(A64_LD) -o $@ build/arm64/start.o $<

test-i64-arm64: build/arm64/i64_test
	@echo "Running I64 test under qemu-aarch64..."
	@$(QEMU_A64) ./build/arm64/i64_test && echo "PASS: i64_test (arm64)" || (echo "FAIL: i64_test (arm64)"; exit 1)

## I64 integration test (IR builder → x86-64 asm → qemu-x86_64)
I64_TEST_X64_SRC := tests/test_i64.c $(IR_SRC) $(BE_X64)

build/test_i64_x64: $(I64_TEST_X64_SRC) ir/ir.h | build
	$(CC) $(CFLAGS) -DX86_BITS=64 -Iir -o $@ $(I64_TEST_X64_SRC)

build/x86_64/i64_test.s: build/test_i64_x64 | build/x86_64
	./build/test_i64_x64 -o $@

build/x86_64/i64_test.o: build/x86_64/i64_test.s $(X64_ASM_DEP)
	$(X64_ASM) $(X64_ASMFLAGS) -o $@ $<

build/x86_64/i64_test: build/x86_64/i64_test.o build/x86_64/start.o
	$(X64_LD) -m elf_x86_64 -o $@ build/x86_64/start.o $<

test-i64-x86-64: build/x86_64/i64_test
	@echo "Running I64 test under qemu-x86_64..."
	@$(QEMU_X64) ./build/x86_64/i64_test && echo "PASS: i64_test (x86-64)" || (echo "FAIL: i64_test (x86-64)"; exit 1)

## Threaded-dispatch integration test (IR builder → x86-64 asm → qemu-x86_64):
## IR_LABADDR + IR_IJMP, the computed-goto primitive, self-checking.
THREADED_TEST_X64_SRC := tests/test_threaded.c $(IR_SRC) $(BE_X64)

build/test_threaded_x64: $(THREADED_TEST_X64_SRC) ir/ir.h | build
	$(CC) $(CFLAGS) -DX86_BITS=64 -Iir -o $@ $(THREADED_TEST_X64_SRC)

build/x86_64/threaded_test.s: build/test_threaded_x64 | build/x86_64
	./build/test_threaded_x64 -o $@

build/x86_64/threaded_test.o: build/x86_64/threaded_test.s $(X64_ASM_DEP)
	$(X64_ASM) $(X64_ASMFLAGS) -o $@ $<

build/x86_64/threaded_test: build/x86_64/threaded_test.o build/x86_64/start.o
	$(X64_LD) -m elf_x86_64 -o $@ build/x86_64/start.o $<

test-threaded-x86-64: build/x86_64/threaded_test
	@echo "Running threaded-dispatch test under qemu-x86_64..."
	@$(QEMU_X64) ./build/x86_64/threaded_test && echo "PASS: threaded_test (x86-64)" || (echo "FAIL: threaded_test (x86-64)"; exit 1)

## Threaded-dispatch test on ColdFire (IR builder → ColdFire asm → qemu-m68k)
THREADED_TEST_CF_SRC := tests/test_threaded.c $(IR_SRC) $(BE_CF)
build/test_threaded_cf: $(THREADED_TEST_CF_SRC) ir/ir.h | build
	$(CC) $(CFLAGS) -Iir -o $@ $(THREADED_TEST_CF_SRC)
build/threaded_test_cf.s: build/test_threaded_cf | build
	./build/test_threaded_cf -o $@
build/threaded_test_cf.o: build/threaded_test_cf.s
	$(M68K_AS) -o $@ $<
build/threaded_test_cf: build/threaded_test_cf.o build/start.o build/skj-ld
	./build/skj-ld -o $@ build/start.o $<
test-threaded: build/threaded_test_cf
	@echo "Running threaded-dispatch test under qemu-m68k..."
	@$(QEMU) ./build/threaded_test_cf && echo "PASS: threaded_test (ColdFire)" || (echo "FAIL: threaded_test (ColdFire)"; exit 1)

## Threaded-dispatch test on RISC-V (IR builder → RISC-V asm → qemu-riscv32)
THREADED_TEST_RV_SRC := tests/test_threaded.c $(IR_SRC) $(BE_RV)
build/test_threaded_rv: $(THREADED_TEST_RV_SRC) ir/ir.h | build
	$(CC) $(CFLAGS) -Iir -o $@ $(THREADED_TEST_RV_SRC)
build/rv/threaded_test.s: build/test_threaded_rv | build/rv
	./build/test_threaded_rv -o $@
build/rv/threaded_test.o: build/rv/threaded_test.s
	$(RV_AS) -march=rv32im -mabi=ilp32 -o $@ $<
build/rv/threaded_test: build/rv/threaded_test.o build/rv/start.o
	$(RV_LD) -m elf32lriscv -o $@ build/rv/start.o $<
test-threaded-rv: build/rv/threaded_test
	@echo "Running threaded-dispatch test under qemu-riscv32..."
	@$(QEMU_RV) ./build/rv/threaded_test && echo "PASS: threaded_test (RISC-V)" || (echo "FAIL: threaded_test (RISC-V)"; exit 1)

## Threaded-dispatch test on arm64 (IR builder → arm64 asm → qemu-aarch64)
THREADED_TEST_A64_SRC := tests/test_threaded.c $(IR_SRC) $(BE_A64)
build/test_threaded_arm64: $(THREADED_TEST_A64_SRC) ir/ir.h | build
	$(CC) $(CFLAGS) -DSKJ_LP64=1 -Iir -o $@ $(THREADED_TEST_A64_SRC)
build/arm64/threaded_test.s: build/test_threaded_arm64 | build/arm64
	./build/test_threaded_arm64 -o $@
build/arm64/threaded_test.o: build/arm64/threaded_test.s $(A64_ASM_DEP)
	$(A64_AS) -o $@ $<
build/arm64/threaded_test: build/arm64/threaded_test.o build/arm64/start.o
	$(A64_LD) -o $@ build/arm64/start.o $<
test-threaded-arm64: build/arm64/threaded_test
	@echo "Running threaded-dispatch test under qemu-aarch64..."
	@$(QEMU_A64) ./build/arm64/threaded_test && echo "PASS: threaded_test (arm64)" || (echo "FAIL: threaded_test (arm64)"; exit 1)

## Threaded-dispatch test on MIPS (IR builder → MIPS asm → qemu-mipsel)
THREADED_TEST_MIPS_SRC := tests/test_threaded.c $(IR_SRC) $(BE_MIPS)
build/test_threaded_mips: $(THREADED_TEST_MIPS_SRC) ir/ir.h | build
	$(CC) $(CFLAGS) -Iir -o $@ $(THREADED_TEST_MIPS_SRC)
build/mips/threaded_test.s: build/test_threaded_mips | build/mips
	./build/test_threaded_mips -o $@
build/mips/threaded_test.o: build/mips/threaded_test.s
	$(MIPS_AS) -march=mips1 -EL -o $@ $<
build/mips/threaded_test: build/mips/threaded_test.o build/mips/start_rt.o
	$(MIPS_LD) -o $@ build/mips/start_rt.o $<
test-threaded-mips: build/mips/threaded_test
	@echo "Running threaded-dispatch test under qemu-mipsel..."
	@$(QEMU_MIPS) ./build/mips/threaded_test && echo "PASS: threaded_test (mips)" || (echo "FAIL: threaded_test (mips)"; exit 1)

## Unsigned 32-bit ops integration test (IR builder → ColdFire asm → qemu-m68k)
OPS_TEST_CF_SRC := tests/test_ops.c $(IR_SRC) $(BE_CF)

build/test_ops_cf: $(OPS_TEST_CF_SRC) ir/ir.h | build
	$(CC) $(CFLAGS) -Iir -o $@ $(OPS_TEST_CF_SRC)

build/ops_test_cf.s: build/test_ops_cf | build
	./build/test_ops_cf -o $@

build/ops_test_cf.o: build/ops_test_cf.s
	$(M68K_AS) -o $@ $<

build/ops_test_cf: build/ops_test_cf.o build/start.o build/skj-ld
	./build/skj-ld -o $@ build/start.o $<

test-ops: build/ops_test_cf
	@echo "Running unsigned ops test under qemu-m68k..."
	@$(QEMU) ./build/ops_test_cf && echo "PASS: ops_test (ColdFire)" || (echo "FAIL: ops_test (ColdFire)"; exit 1)

## Unsigned 32-bit ops integration test (IR builder → RISC-V asm → qemu-riscv32)
OPS_TEST_RV_SRC := tests/test_ops.c $(IR_SRC) $(BE_RV)

build/test_ops_rv: $(OPS_TEST_RV_SRC) ir/ir.h | build
	$(CC) $(CFLAGS) -Iir -o $@ $(OPS_TEST_RV_SRC)

build/rv/ops_test.s: build/test_ops_rv | build/rv
	./build/test_ops_rv -o $@

build/rv/ops_test.o: build/rv/ops_test.s
	$(RV_AS) -march=rv32im -mabi=ilp32 -o $@ $<

build/rv/ops_test: build/rv/ops_test.o build/rv/start.o
	$(RV_LD) -m elf32lriscv -o $@ build/rv/start.o $<

test-ops-rv: build/rv/ops_test
	@echo "Running unsigned ops test under qemu-riscv32..."
	@$(QEMU_RV) ./build/rv/ops_test && echo "PASS: ops_test (RISC-V)" || (echo "FAIL: ops_test (RISC-V)"; exit 1)

## Unsigned 32-bit ops integration test (IR builder → arm64 asm → qemu-aarch64)
OPS_TEST_A64_SRC := tests/test_ops.c $(IR_SRC) $(BE_A64)

build/test_ops_arm64: $(OPS_TEST_A64_SRC) ir/ir.h | build
	$(CC) $(CFLAGS) -DSKJ_LP64=1 -Iir -o $@ $(OPS_TEST_A64_SRC)

build/arm64/ops_test.s: build/test_ops_arm64 | build/arm64
	./build/test_ops_arm64 -o $@

build/arm64/ops_test.o: build/arm64/ops_test.s $(A64_ASM_DEP)
	$(A64_AS) -o $@ $<

build/arm64/ops_test: build/arm64/ops_test.o build/arm64/start.o
	$(A64_LD) -o $@ build/arm64/start.o $<

test-ops-arm64: build/arm64/ops_test
	@echo "Running unsigned ops test under qemu-aarch64..."
	@$(QEMU_A64) ./build/arm64/ops_test && echo "PASS: ops_test (arm64)" || (echo "FAIL: ops_test (arm64)"; exit 1)

## Unsigned 32-bit ops integration test (IR builder → x86-64 asm → qemu-x86_64)
OPS_TEST_X64_SRC := tests/test_ops.c $(IR_SRC) $(BE_X64)

build/test_ops_x64: $(OPS_TEST_X64_SRC) ir/ir.h | build
	$(CC) $(CFLAGS) -DX86_BITS=64 -Iir -o $@ $(OPS_TEST_X64_SRC)

build/x86_64/ops_test.s: build/test_ops_x64 | build/x86_64
	./build/test_ops_x64 -o $@

build/x86_64/ops_test.o: build/x86_64/ops_test.s $(X64_ASM_DEP)
	$(X64_ASM) $(X64_ASMFLAGS) -o $@ $<

build/x86_64/ops_test: build/x86_64/ops_test.o build/x86_64/start.o
	$(X64_LD) -m elf_x86_64 -o $@ build/x86_64/start.o $<

test-ops-x86-64: build/x86_64/ops_test
	@echo "Running unsigned ops test under qemu-x86_64..."
	@$(QEMU_X64) ./build/x86_64/ops_test && echo "PASS: ops_test (x86-64)" || (echo "FAIL: ops_test (x86-64)"; exit 1)


## FPU integration test (IR builder → arm64 asm → qemu-aarch64).
## _Float16 FLH/FSH are native fcvt on AArch64, so no half.o helper is linked.
FPU_TEST_A64_SRC := tests/test_fpu.c $(IR_SRC) $(BE_A64)

build/test_fpu_arm64: $(FPU_TEST_A64_SRC) ir/ir.h | build
	$(CC) $(CFLAGS) -DSKJ_LP64=1 -Iir -o $@ $(FPU_TEST_A64_SRC)

build/arm64/fpu_test.s: build/test_fpu_arm64 | build/arm64
	./build/test_fpu_arm64 -o $@

build/arm64/fpu_test.o: build/arm64/fpu_test.s $(A64_ASM_DEP)
	$(A64_AS) -o $@ $<

build/arm64/fpu_test: build/arm64/fpu_test.o build/arm64/start.o
	$(A64_LD) -o $@ build/arm64/start.o $<

test-fpu-arm64: build/arm64/fpu_test
	@echo "Running FPU test under qemu-aarch64..."
	@$(QEMU_A64) ./build/arm64/fpu_test && echo "PASS: fpu_test (arm64)" || (echo "FAIL: fpu_test (arm64)"; exit 1)

## FPU integration test (IR builder → RISC-V asm → qemu-riscv32).
## _Float16 FLH/FSH are native Zfh fcvt on RV, so no half.o helper is linked.
## The float test object assembles as rv32imfd_zfh (an explicit rule, since the
## generic build/rv/%.o pattern is integer-only rv32im).
FPU_TEST_RV_SRC := tests/test_fpu.c $(IR_SRC) $(BE_RV)

build/test_fpu_rv: $(FPU_TEST_RV_SRC) ir/ir.h | build
	$(CC) $(CFLAGS) -Iir -o $@ $(FPU_TEST_RV_SRC)

build/rv/fpu_test.s: build/test_fpu_rv | build/rv
	./build/test_fpu_rv -o $@

build/rv/fpu_test.o: build/rv/fpu_test.s
	$(RV_AS) -march=rv32imfd_zfh -mabi=ilp32 -o $@ $<

build/rv/fpu_test: build/rv/fpu_test.o build/rv/start.o
	$(RV_LD) -m elf32lriscv -o $@ build/rv/start.o $<

test-fpu-rv: build/rv/fpu_test
	@echo "Running FPU test under qemu-riscv32..."
	@$(QEMU_RV) ./build/rv/fpu_test && echo "PASS: fpu_test (RISC-V)" || (echo "FAIL: fpu_test (RISC-V)"; exit 1)

## FPU integration test (IR builder → x86-64 asm → qemu-x86_64).
## _Float16 FLH/FSH call the half.c helpers via the SysV register ABI, so
## half.o is compiled natively (64-bit) here.
FPU_TEST_X64_SRC := tests/test_fpu.c $(IR_SRC) $(BE_X64)

build/test_fpu_x64: $(FPU_TEST_X64_SRC) ir/ir.h | build
	$(CC) $(CFLAGS) -DX86_BITS=64 -Iir -o $@ $(FPU_TEST_X64_SRC)

build/x86_64/fpu_test.s: build/test_fpu_x64 | build/x86_64
	./build/test_fpu_x64 -o $@

build/x86_64/fpu_test.o: build/x86_64/fpu_test.s $(X64_ASM_DEP)
	$(X64_ASM) $(X64_ASMFLAGS) -o $@ $<

build/x86_64/half.o: runtime/half.c | build/x86_64
	$(CC) -std=c99 -O2 -Wall -ffreestanding -c -o $@ $<

## 64-bit div/mod helpers (__udivdi3 and family).  Even the native-i64 targets
## lower 64-bit division to these calls, so a libc using 64-bit arithmetic
## (printf's %lld path) needs them; soft64.c supplies them in portable C.
build/x86_64/soft64.o: runtime/soft64.c | build/x86_64
	$(CC) -std=c99 -O2 -Wall -ffreestanding -DSOFT64_NO_MULSHIFT -c -o $@ $<

build/x86_64/fpu_test: build/x86_64/fpu_test.o build/x86_64/start.o build/x86_64/half.o
	$(X64_LD) -m elf_x86_64 -o $@ build/x86_64/start.o build/x86_64/half.o $<

test-fpu-x86-64: build/x86_64/fpu_test
	@echo "Running FPU test under qemu-x86_64..."
	@$(QEMU_X64) ./build/x86_64/fpu_test && echo "PASS: fpu_test (x86-64)" || (echo "FAIL: fpu_test (x86-64)"; exit 1)

## Native single-precision (IR_F32) integration test on x86-64
F32_TEST_X64_SRC := tests/test_f32.c $(IR_SRC) $(BE_X64)
build/test_f32_x64: $(F32_TEST_X64_SRC) ir/ir.h | build
	$(CC) $(CFLAGS) -DX86_BITS=64 -Iir -o $@ $(F32_TEST_X64_SRC)
build/x86_64/f32_test.s: build/test_f32_x64 | build/x86_64
	./build/test_f32_x64 -o $@
build/x86_64/f32_test.o: build/x86_64/f32_test.s $(X64_ASM_DEP)
	$(X64_ASM) $(X64_ASMFLAGS) -o $@ $<
build/x86_64/f32_test: build/x86_64/f32_test.o build/x86_64/start.o
	$(X64_LD) -m elf_x86_64 -o $@ build/x86_64/start.o $<
test-f32-x86-64: build/x86_64/f32_test
	@echo "Running F32 test under qemu-x86_64..."
	@$(QEMU_X64) ./build/x86_64/f32_test && echo "PASS: f32_test (x86-64)" || (echo "FAIL: f32_test (x86-64)"; exit 1)

F32_TEST_A64_SRC := tests/test_f32.c $(IR_SRC) $(BE_A64)
build/test_f32_arm64: $(F32_TEST_A64_SRC) ir/ir.h | build
	$(CC) $(CFLAGS) -DSKJ_LP64=1 -Iir -o $@ $(F32_TEST_A64_SRC)
build/arm64/f32_test.s: build/test_f32_arm64 | build/arm64
	./build/test_f32_arm64 -o $@
build/arm64/f32_test.o: build/arm64/f32_test.s $(A64_ASM_DEP)
	$(A64_AS) -o $@ $<
build/arm64/f32_test: build/arm64/f32_test.o build/arm64/start.o
	$(A64_LD) -o $@ build/arm64/start.o $<
test-f32-arm64: build/arm64/f32_test
	@echo "Running F32 test under qemu-aarch64..."
	@$(QEMU_A64) ./build/arm64/f32_test && echo "PASS: f32_test (arm64)" || (echo "FAIL: f32_test (arm64)"; exit 1)

F32_TEST_RV_SRC := tests/test_f32.c $(IR_SRC) $(BE_RV)
build/test_f32_rv: $(F32_TEST_RV_SRC) ir/ir.h | build
	$(CC) $(CFLAGS) -Iir -o $@ $(F32_TEST_RV_SRC)
build/rv/f32_test.s: build/test_f32_rv | build/rv
	./build/test_f32_rv -o $@
build/rv/f32_test.o: build/rv/f32_test.s
	$(RV_AS) -march=rv32imfd_zfh -mabi=ilp32 -o $@ $<
build/rv/f32_test: build/rv/f32_test.o build/rv/start.o
	$(RV_LD) -m elf32lriscv -o $@ build/rv/start.o $<
test-f32-rv: build/rv/f32_test
	@echo "Running F32 test under qemu-riscv32..."
	@$(QEMU_RV) ./build/rv/f32_test && echo "PASS: f32_test (RISC-V)" || (echo "FAIL: f32_test (RISC-V)"; exit 1)

## C preprocessor tests
# cc_t071 uses the __builtin_va_arg_* accessors and is excluded on every
# target; the portable <stdarg.h> coverage is the VA_STDARG test below.
VARARGS_EXCLUDE   := cc_t071_varargs
# Every target passes and returns a struct by value, including MIPS o32 (a
# struct with 8-byte-aligned members starts on an even argument word, matching
# its 8-aligned home slot), so the struct tests run on every target.
CF_EXCLUDE        := $(VARARGS_EXCLUDE)
MIPS_EXCLUDE      := $(VARARGS_EXCLUDE)
RV_EXCLUDE        := $(VARARGS_EXCLUDE)
# The portable <stdarg.h> test runs on ColdFire, MIPS hard-float, x86-64 and
# arm64.  It is excluded only where the varargs backend is not implemented
# (RISC-V) or where a double vararg is not supported (MIPS soft-float).
VA_STDARG_EXCLUDE := cc_t081_varargs_stdarg

check-cc: build/skj-cc build/skj-as build/start.o build/half.o build/va_cf.o
	@OUTDIR="$(CURDIR)/build/cc-cf" EXCLUDE="$(CF_EXCLUDE)" \
	 HALF="$(CURDIR)/build/half.o $(CURDIR)/build/va_cf.o" \
	 sh tests/run-cc-tests.sh

## C compiler tests on x86-64 (skj-cc-x86-64, qemu-x86_64).  This is where
## native single-precision float rounding is actually exercised.
check-cc-x86-64: build/skj-cc-x86-64 build/x86_64/start_sysv.o build/x86_64/half.o build/x86_64/va.o
	@CCBIN="$(CURDIR)/build/skj-cc-x86-64" \
	 ASM="$(X64_ASM) $(X64_ASMFLAGS) -o" \
	 LD="$(X64_LD) -m elf_x86_64 -o" \
	 QEMU="$(QEMU_X64)" \
	 START="$(CURDIR)/build/x86_64/start_sysv.o" \
	 HALF="$(CURDIR)/build/x86_64/half.o $(CURDIR)/build/x86_64/va.o" \
	 OUTDIR="$(CURDIR)/build/cc-x86_64" \
	 sh tests/run-cc-tests.sh

## C compiler tests on arm64 (skj-cc-arm64, qemu-aarch64).  _Float16 is a
## native fcvt here, so no half.o is linked (HALF empty).
check-cc-arm64: build/skj-cc-arm64 build/arm64/start_aapcs.o build/arm64/va.o
	@CCBIN="$(CURDIR)/build/skj-cc-arm64" \
	 ASM="$(A64_AS) -o" \
	 LD="$(A64_LD) -o" \
	 QEMU="$(QEMU_A64)" \
	 START="$(CURDIR)/build/arm64/start_aapcs.o" \
	 HALF="$(CURDIR)/build/arm64/va.o" \
	 OUTDIR="$(CURDIR)/build/cc-arm64" \
	 sh tests/run-cc-tests.sh

## C compiler tests on RISC-V (skj-cc-rv, qemu-riscv32).  The RV backend has
## hardware float (RV32FD) plus Zfh for _Float16, so the full cc suite runs;
## the test objects assemble as rv32imfd_zfh (start.o stays rv32im, a subset
## the linker merges).
check-cc-rv: build/skj-cc-rv build/rv/start.o build/rv/va_rv.o
	@CCBIN="$(CURDIR)/build/skj-cc-rv" \
	 ASM="$(RV_AS) -march=rv32imfd_zfh -mabi=ilp32 -o" \
	 LD="$(RV_LD) -m elf32lriscv -o" \
	 QEMU="$(QEMU_RV)" \
	 START="$(CURDIR)/build/rv/start.o" \
	 HALF="$(CURDIR)/build/rv/va_rv.o" \
	 OUTDIR="$(CURDIR)/build/cc-rv" \
	 EXCLUDE="$(RV_EXCLUDE)" \
	 sh tests/run-cc-tests.sh

## The same C suite, but assembled and linked entirely by the in-tree RV32
## toolchain (skj-as-rv + skj-ld-rv) over a skj-assembled crt, run under
## qemu-riscv32.  This is the end-to-end check that skj-as-rv/skj-ld-rv can
## build a full program with its runtime, not just match GNU as byte-for-byte.
build/rv-skj:
	mkdir -p build/rv-skj

build/rv-skj/start.o: runtime/start_rv.S build/skj-as-rv | build/rv-skj
	./build/skj-as-rv -o $@ $<

## The psABI runtime for the skj-toolchain variant: the C parts are
## gcc-built, but -fno-pic -mno-relax keeps them to the relocations skj-ld-rv
## implements (no GOT, no linker relaxation; the SET*/SUB* pairs stay in the
## discarded .eh_frame).  The crt is assembled by skj-as-rv.
RV_SKJ_RT_CFLAGS := $(RV_RT_CFLAGS) -fno-pic -mno-relax

build/rv-skj/%.o: runtime/%.c build/skj-as-rv | build/rv-skj
	$(RV_CC) $(RV_SKJ_RT_CFLAGS) -c -o $@ $<

check-cc-rv-skj: build/skj-cc-rv build/skj-as-rv build/skj-ld-rv build/rv-skj/start.o build/rv-skj/va_rv.o
	@CCBIN="$(CURDIR)/build/skj-cc-rv" \
	 ASM="$(CURDIR)/build/skj-as-rv -o" \
	 LD="$(CURDIR)/build/skj-ld-rv -o" \
	 QEMU="$(QEMU_RV)" \
	 START="$(CURDIR)/build/rv-skj/start.o" \
	 HALF="$(CURDIR)/build/rv-skj/va_rv.o" \
	 OUTDIR="$(CURDIR)/build/cc-rv-skj" \
	 EXCLUDE="$(RV_EXCLUDE)" \
	 sh tests/run-cc-tests.sh

## The RV32 process sandbox: skj-run runs a root guest that spawns a child
## from an embedded image, under an aggregate memory budget the scheduler
## shares across processes.  The demo guests are built by the in-tree skj
## toolchain (skj-cc-rv + skj-as-rv + skj-ld-rv over the start_rv crt); the
## child ELF is embedded in the parent as a byte array.  This exercises the
## guest-memory primitives in their real consumer: gm_read_out and the
## in-memory ELF loader (spawn), gm_mmap/gm_munmap (the guarded parent map and
## child teardown), the gm_pool aggregate cap (--total-mem), and the batched
## rv_run reason contract (the QUANTUM scheduler resuming a child over many
## slices).  The unit tests test-rv-run and test-guest-mmap pin the same
## primitives in isolation.
build/sandbox:
	mkdir -p build/sandbox

build/sandbox/sys.o: tests/sandbox/sys.S build/skj-as-rv | build/sandbox
	./build/skj-as-rv -o $@ $<

build/sandbox/child.o: tests/sandbox/child.c build/skj-cc-rv build/skj-as-rv | build/sandbox
	./build/skj-cc-rv -o build/sandbox/child.s $<
	./build/skj-as-rv -o $@ build/sandbox/child.s

build/sandbox/child.elf: build/sandbox/child.o build/sandbox/sys.o build/rv-skj/start.o build/skj-ld-rv
	./build/skj-ld-rv -o $@ build/rv-skj/start.o build/sandbox/child.o build/sandbox/sys.o

build/sandbox/child_image.h: build/sandbox/child.elf
	printf 'static const unsigned char child_img[] = {\n' > $@
	xxd -i < build/sandbox/child.elf >> $@
	printf '};\n#define CHILD_IMG_LEN %uu\n' "$$(wc -c < build/sandbox/child.elf)" >> $@

build/sandbox/parent.o: tests/sandbox/parent.c build/sandbox/child_image.h build/skj-cc-rv build/skj-as-rv | build/sandbox
	./build/skj-cc-rv -Ibuild/sandbox -o build/sandbox/parent.s $<
	./build/skj-as-rv -o $@ build/sandbox/parent.s

build/sandbox/parent.elf: build/sandbox/parent.o build/sandbox/sys.o build/rv-skj/start.o build/skj-ld-rv
	./build/skj-ld-rv -o $@ build/rv-skj/start.o build/sandbox/parent.o build/sandbox/sys.o

test-rv-sandbox: build/sandbox/parent.elf build/skj-run
	@sh tests/run-sandbox-test.sh "$(CURDIR)/build/skj-run" "$(CURDIR)/build/sandbox/parent.elf"

## The cross-language sandbox demo: a C root (skj-cc-rv) spawns two children,
## one built from C and one from Kobold-Pascal (skj-kpc -> skj-kp-rv ->
## skj-as-rv -> skj-ld-rv over the freestanding crt + libkpaot-rv-free.a).  All
## three are RV32 ELFs the toolkit produced from the one shared IR, so skj-run
## schedules a C process and a Pascal process side by side with nothing
## language-specific in the runtime.  This is the companion of test-rv-sandbox,
## making the "one IR, many front ends, one runtime" claim concrete across two
## source languages at once.  Both children run the same sum 1..99 = 4950.
build/xlang-sandbox:
	mkdir -p build/xlang-sandbox

build/xlang-sandbox/sys.o: tests/xlang-sandbox/sys.S build/skj-as-rv | build/xlang-sandbox
	./build/skj-as-rv -o $@ $<

build/xlang-sandbox/cchild.o: tests/xlang-sandbox/cchild.c build/skj-cc-rv build/skj-as-rv | build/xlang-sandbox
	./build/skj-cc-rv -o build/xlang-sandbox/cchild.s $<
	./build/skj-as-rv -o $@ build/xlang-sandbox/cchild.s

build/xlang-sandbox/cchild.elf: build/xlang-sandbox/cchild.o build/rv-skj/start.o build/skj-ld-rv
	./build/skj-ld-rv -o $@ build/rv-skj/start.o build/xlang-sandbox/cchild.o

build/xlang-sandbox/kchild.kpm: tests/xlang-sandbox/kchild.pas build/skj-kpc-rv | build/xlang-sandbox
	./build/skj-kpc-rv --emit-kpm $< > $@

build/xlang-sandbox/kchild.o: build/xlang-sandbox/kchild.kpm build/skj-kp-rv build/skj-as-rv
	./build/skj-kp-rv -o build/xlang-sandbox/kchild.s $<
	./build/skj-as-rv -o $@ build/xlang-sandbox/kchild.s

build/xlang-sandbox/kchild.elf: build/xlang-sandbox/kchild.o build/kpfree-rv/kp_crt_rv.o build/libkpaot-rv-free.a build/skj-ld-rv
	./build/skj-ld-rv -o $@ build/kpfree-rv/kp_crt_rv.o build/xlang-sandbox/kchild.o build/libkpaot-rv-free.a

build/xlang-sandbox/images.h: build/xlang-sandbox/cchild.elf build/xlang-sandbox/kchild.elf
	printf 'static const unsigned char cchild_img[] = {\n' > $@
	xxd -i < build/xlang-sandbox/cchild.elf >> $@
	printf '};\n#define CCHILD_LEN %uu\n' "$$(wc -c < build/xlang-sandbox/cchild.elf)" >> $@
	printf 'static const unsigned char kchild_img[] = {\n' >> $@
	xxd -i < build/xlang-sandbox/kchild.elf >> $@
	printf '};\n#define KCHILD_LEN %uu\n' "$$(wc -c < build/xlang-sandbox/kchild.elf)" >> $@

build/xlang-sandbox/parent.o: tests/xlang-sandbox/parent.c build/xlang-sandbox/images.h build/skj-cc-rv build/skj-as-rv | build/xlang-sandbox
	./build/skj-cc-rv -Ibuild/xlang-sandbox -o build/xlang-sandbox/parent.s $<
	./build/skj-as-rv -o $@ build/xlang-sandbox/parent.s

build/xlang-sandbox/parent.elf: build/xlang-sandbox/parent.o build/xlang-sandbox/sys.o build/rv-skj/start.o build/skj-ld-rv
	./build/skj-ld-rv -o $@ build/rv-skj/start.o build/xlang-sandbox/parent.o build/xlang-sandbox/sys.o

test-rv-xlang-sandbox: build/xlang-sandbox/parent.elf build/skj-run
	@sh tests/run-xlang-sandbox.sh "$(CURDIR)/build/skj-run" "$(CURDIR)/build/xlang-sandbox/parent.elf"

## The psABI C suite end to end through the in-tree RV32 toolchain:
## skj-cc-rv emits the standard RISC-V ILP32 convention, assembled by
## skj-as-rv and linked by skj-ld-rv over the psABI crt, run under qemu-riscv32.
## The crt (start_rv_psabi_cc.o) is assembled by skj-as-rv.  varargs and
## by-value structs are still excluded: the RV backend implements the scalar
## psABI (a0..a7, i64 register pairs, results in a0/a1), not the struct-passing
## rules, so those tests wait on RV struct-ABI support.
build/rv-skj/start_psabi_cc.o: runtime/start_rv_psabi_cc.S build/skj-as-rv | build/rv-skj
	./build/skj-as-rv -o $@ $<

check-cc-rv-psabi: build/skj-cc-rv build/skj-as-rv build/skj-ld-rv build/rv-skj/start_psabi_cc.o build/rv-skj/va_rv.o
	@CCBIN="$(CURDIR)/build/skj-cc-rv" \
	 ASM="$(CURDIR)/build/skj-as-rv -o" \
	 LD="$(CURDIR)/build/skj-ld-rv -o" \
	 QEMU="$(QEMU_RV)" \
	 START="$(CURDIR)/build/rv-skj/start_psabi_cc.o" \
	 HALF="$(CURDIR)/build/rv-skj/va_rv.o" \
	 OUTDIR="$(CURDIR)/build/cc-rv-psabi" \
	 EXCLUDE="$(RV_EXCLUDE)" \
	 sh tests/run-cc-tests.sh

## psABI interop proof: a skj-cc-rv main calls a gcc-built RV32 function
## (i64 and int arguments, i64 result) and the whole program is linked by
## skj-ld-rv.  This is the point of the psABI build, that its output follows
## the standard ABI a stock RISC-V toolchain emits.
test-cc-rv-psabi-interop: build/skj-cc-rv build/skj-as-rv build/skj-ld-rv build/rv-skj/start_psabi_cc.o | build/rv-skj
	@$(RV_CC) $(RV_SKJ_RT_CFLAGS) -c -o build/rv-skj/interop_gcc.o tests/rv_psabi_interop_gcc.c
	@./build/skj-cc-rv -o build/rv-skj/interop_main.s tests/rv_psabi_interop_main.c
	@./build/skj-as-rv -o build/rv-skj/interop_main.o build/rv-skj/interop_main.s
	@./build/skj-ld-rv -o build/rv-skj/interop build/rv-skj/start_psabi_cc.o \
	    build/rv-skj/interop_main.o build/rv-skj/interop_gcc.o
	@$(QEMU_RV) ./build/rv-skj/interop; ec=$$?; \
	 if [ $$ec -eq 42 ]; then echo "PASS: cc-rv-psabi interop (skj main + gcc fn, exit 42)"; \
	 else echo "FAIL: cc-rv-psabi interop (exit $$ec, expected 42)"; exit 1; fi

## By-value struct interop: skj-cc-rv passes and returns structs by value to and
## from gcc-built RV32 objects through the ilp32 psABI, in both directions (a
## 2-word struct in registers and a >8-byte struct by reference), and gcc calls
## back into a skj-built function with a by-value struct.  The direct proof that
## skj-cc-rv's struct ABI matches a stock RISC-V toolchain.
test-cc-rv-psabi-struct: build/skj-cc-rv build/skj-as-rv build/skj-ld-rv build/rv-skj/start_psabi_cc.o | build/rv-skj
	@$(RV_CC) $(RV_SKJ_RT_CFLAGS) -Itests -c -o build/rv-skj/struct_gcc.o tests/rv_struct_interop_gcc.c
	@./build/skj-cc-rv -Itests -o build/rv-skj/struct_main.s tests/rv_struct_interop_main.c
	@./build/skj-as-rv -o build/rv-skj/struct_main.o build/rv-skj/struct_main.s
	@./build/skj-ld-rv -o build/rv-skj/struct build/rv-skj/start_psabi_cc.o \
	    build/rv-skj/struct_main.o build/rv-skj/struct_gcc.o
	@$(QEMU_RV) ./build/rv-skj/struct; ec=$$?; \
	 if [ $$ec -eq 42 ]; then echo "PASS: cc-rv-psabi struct interop (skj <-> gcc, by-value + return, exit 42)"; \
	 else echo "FAIL: cc-rv-psabi struct interop (exit $$ec, expected 42)"; exit 1; fi

## Cross-driver (skjcc) library staging: the crt and runtime helpers skjcc links
## per target, copied into build/lib/<target>/ (the compiled-in lib dir).  The
## source objects are the same per-target crt/runtime the check-cc-* suites use.
build/lib/coldfire/crt0.o: build/start.o
	@mkdir -p $(@D) && cp $< $@
build/lib/coldfire/half.o: build/half.o
	@mkdir -p $(@D) && cp $< $@
build/lib/coldfire/va_cf.o: build/va_cf.o
	@mkdir -p $(@D) && cp $< $@
build/lib/riscv/crt0.o: build/rv/start.o
	@mkdir -p $(@D) && cp $< $@
build/lib/riscv/va_rv.o: build/rv/va_rv.o
	@mkdir -p $(@D) && cp $< $@
build/lib/mips/crt0.o: build/mips/start.o
	@mkdir -p $(@D) && cp $< $@
build/lib/mips/half.o: build/mips/half.o
	@mkdir -p $(@D) && cp $< $@
build/lib/mips/va_mips.o: build/mips/va_mips.o
	@mkdir -p $(@D) && cp $< $@

## soft64 divide helpers (__udivdi3/__umoddi3/...) the rv/mips crts do not carry
## but the libc's 64-bit formatting (and any user 64-bit divide) needs.  The cf
## crt already defines the whole family, so only rv/mips stage it.
build/lib/riscv/soft64_div.o: build/rv/soft64_div.o
	@mkdir -p $(@D) && cp $< $@
build/lib/mips/soft64_div.o: build/mips/soft64_div.o
	@mkdir -p $(@D) && cp $< $@

## The coroutine context switch (co_switch), the one arch-specific piece of the
## libc coroutine primitive (libc/coro.c), assembled per target.  The rv, x86-64
## and arm64 switches already exist for the Kobold runtime and are reused as-is;
## ColdFire and MIPS get their own in runtime/.  Each is archived into that
## target's libc.a (so a program pulls it only when it uses a coroutine) and
## also linked loose by the libc test harness.
build/co_switch_cf.o: runtime/co_switch_cf.S build/skj-as | build
	@./build/skj-as -o $@ $<
build/rv/co_switch.o: runtime/co_switch_rv.S build/skj-as-rv | build/rv
	@./build/skj-as-rv -o $@ $<
build/mips/co_switch.o: runtime/co_switch_mips.S build/skj-as-mips | build/mips
	@./build/skj-as-mips -o $@ $<
build/x86_64/co_switch.o: runtime/co_switch_x86.S | build/x86_64
	@$(CC) -c $< -o $@
build/arm64/co_switch.o: runtime/co_switch_arm64.S | build/arm64
	@$(A64_CC) -c $< -o $@

## The libc archive (doc/tools/libc.md Phase D): the Phase A-C sources compiled per
## target with that target's skj-cc, assembled with the in-tree assembler, and
## archived by skj-ar with a symbol index so the linker pulls only what a hosted
## program references.  skjcc links it by default; -ffreestanding omits it.
## stat/fstat go through statx on every target (the newer syscall arches have
## no bare fstat/stat), so each crt carries a statx wrapper and the one
## libc/stat.c serves them all.
LIBC_SRCS := string ctype stdlib printf numfmt stdio_core stdio memstream assert malloc sbrk errno \
             strerror rand arc4random strtod scanf coro threads stat fileops dirent time getenv
LIBC_SRC_FILES := $(addprefix libc/,$(addsuffix .c,$(LIBC_SRCS)))
LIBC_HDRS := $(wildcard include/*.h) $(wildcard include/sys/*.h) libc/fmt.h libc/stdio_impl.h

## printf's floating conversions (f F e E g G a A) are a compile-time option
## (SKJ_LIBC_FLOAT in libc/printf.c): defined here, the shipped libc.a formats
## floats; omitted, it links an integer-only printf with no double arithmetic.
## On everywhere today.  To ship a float-free libc for a target that cannot run
## double (for example the RV32 skj-run core, which has no D extension), clear
## that target's variable below.
LIBC_CFLAGS_CF   := -DSKJ_LIBC_FLOAT
LIBC_CFLAGS_RV   := -DSKJ_LIBC_FLOAT
LIBC_CFLAGS_MIPS := -DSKJ_LIBC_FLOAT

build/setjmp_cf.o: runtime/setjmp_cf.S build/skj-as | build
	./build/skj-as -o $@ $<

## rand/srand and arc4random draw from the shared generators (runtime/rng.c),
## so that one pure object ships in libc as well as the Kobold archives.
build/lib/coldfire/obj/rng.o: runtime/rng.c runtime/rng.h build/skj-cc build/skj-as | build
	@mkdir -p build/lib/coldfire/obj
	@./build/skj-cc $(LIBC_CFLAGS_CF) -o build/lib/coldfire/obj/rng.s runtime/rng.c
	@./build/skj-as -o $@ build/lib/coldfire/obj/rng.s

build/lib/coldfire/libc.a: build/skj-cc build/skj-as build/skj-ar \
                           $(LIBC_SRC_FILES) $(LIBC_HDRS) build/lib/coldfire/obj/rng.o build/co_switch_cf.o build/setjmp_cf.o | build
	@mkdir -p build/lib/coldfire/obj
	@for s in $(LIBC_SRCS); do \
	    ./build/skj-cc $(LIBC_CFLAGS_CF) -o build/lib/coldfire/obj/$$s.s libc/$$s.c && \
	    ./build/skj-as -o build/lib/coldfire/obj/$$s.o build/lib/coldfire/obj/$$s.s \
	    || exit 1; \
	done
	@rm -f $@
	@./build/skj-ar rcs $@ $(addprefix build/lib/coldfire/obj/,$(addsuffix .o,$(LIBC_SRCS))) build/lib/coldfire/obj/rng.o build/co_switch_cf.o build/setjmp_cf.o

build/rv/setjmp.o: runtime/setjmp_rv.S build/skj-as-rv | build/rv
	./build/skj-as-rv -o $@ $<

build/lib/riscv/obj/rng.o: runtime/rng.c runtime/rng.h build/skj-cc-rv build/skj-as-rv | build
	@mkdir -p build/lib/riscv/obj
	@./build/skj-cc-rv $(LIBC_CFLAGS_RV) -o build/lib/riscv/obj/rng.s runtime/rng.c
	@./build/skj-as-rv -o $@ build/lib/riscv/obj/rng.s

build/lib/riscv/libc.a: build/skj-cc-rv build/skj-as-rv build/skj-ar \
                        $(LIBC_SRC_FILES) $(LIBC_HDRS) build/lib/riscv/obj/rng.o build/rv/co_switch.o build/rv/setjmp.o | build
	@mkdir -p build/lib/riscv/obj
	@for s in $(LIBC_SRCS); do \
	    ./build/skj-cc-rv $(LIBC_CFLAGS_RV) -o build/lib/riscv/obj/$$s.s libc/$$s.c && \
	    ./build/skj-as-rv -o build/lib/riscv/obj/$$s.o build/lib/riscv/obj/$$s.s \
	    || exit 1; \
	done
	@rm -f $@
	@./build/skj-ar rcs $@ $(addprefix build/lib/riscv/obj/,$(addsuffix .o,$(LIBC_SRCS))) build/lib/riscv/obj/rng.o build/rv/co_switch.o build/rv/setjmp.o

build/mips/setjmp.o: runtime/setjmp_mips.S build/skj-as-mips | build/mips
	./build/skj-as-mips -o $@ $<

build/lib/mips/obj/rng.o: runtime/rng.c runtime/rng.h build/skj-cc-mips build/skj-as-mips | build
	@mkdir -p build/lib/mips/obj
	@./build/skj-cc-mips $(LIBC_CFLAGS_MIPS) -o build/lib/mips/obj/rng.s runtime/rng.c
	@./build/skj-as-mips -o $@ build/lib/mips/obj/rng.s

build/lib/mips/libc.a: build/skj-cc-mips build/skj-as-mips build/skj-ar \
                       $(LIBC_SRC_FILES) $(LIBC_HDRS) build/lib/mips/obj/rng.o build/mips/co_switch.o build/mips/setjmp.o | build
	@mkdir -p build/lib/mips/obj
	@for s in $(LIBC_SRCS); do \
	    ./build/skj-cc-mips $(LIBC_CFLAGS_MIPS) -o build/lib/mips/obj/$$s.s libc/$$s.c && \
	    ./build/skj-as-mips -o build/lib/mips/obj/$$s.o build/lib/mips/obj/$$s.s \
	    || exit 1; \
	done
	@rm -f $@
	@./build/skj-ar rcs $@ $(addprefix build/lib/mips/obj/,$(addsuffix .o,$(LIBC_SRCS))) build/lib/mips/obj/rng.o build/mips/co_switch.o build/mips/setjmp.o

## fini.o: exit/atexit and the finalizer the crt calls when main returns.  It
## is force-linked (not archived) so the crt's weak __libc_fini reference
## always resolves in a hosted link and the atexit/flush path is present even
## when the program never calls exit; -ffreestanding omits it.
build/lib/coldfire/fini.o: build/skj-cc build/skj-as libc/fini.c $(LIBC_HDRS) | build
	@mkdir -p $(@D)
	@./build/skj-cc -o $(@D)/fini.s libc/fini.c
	@./build/skj-as -o $@ $(@D)/fini.s
build/lib/riscv/fini.o: build/skj-cc-rv build/skj-as-rv libc/fini.c $(LIBC_HDRS) | build
	@mkdir -p $(@D)
	@./build/skj-cc-rv -o $(@D)/fini.s libc/fini.c
	@./build/skj-as-rv -o $@ $(@D)/fini.s
build/lib/mips/fini.o: build/skj-cc-mips build/skj-as-mips libc/fini.c $(LIBC_HDRS) | build
	@mkdir -p $(@D)
	@./build/skj-cc-mips -o $(@D)/fini.s libc/fini.c
	@./build/skj-as-mips -o $@ $(@D)/fini.s

## The cross driver end to end: build and run a self-checking program (exit 42)
## for each in-tree (binutils-free) target through the one skjcc command, plus
## the -S / -c stages and a two-file link.
test-cc-driver: build/skjcc \
                build/skj-cc build/skj-as build/skj-ld $(DRIVER_LIB_CF) \
                build/skj-cc-rv build/skj-as-rv build/skj-ld-rv $(DRIVER_LIB_RV) \
                build/skj-cc-mips build/skj-as-mips build/skj-ld-mips $(DRIVER_LIB_MIPS) | build
	@mkdir -p build/driver
	@./build/skjcc -target coldfire -o build/driver/hello_cf tests/driver_hello.c
	@$(QEMU) ./build/driver/hello_cf; ec=$$?; \
	 [ $$ec -eq 42 ] || { echo "FAIL: driver coldfire (exit $$ec)"; exit 1; }
	@./build/skjcc -target riscv -o build/driver/hello_rv tests/driver_hello.c
	@$(QEMU_RV) ./build/driver/hello_rv; ec=$$?; \
	 [ $$ec -eq 42 ] || { echo "FAIL: driver riscv (exit $$ec)"; exit 1; }
	@./build/skjcc -target mips -o build/driver/hello_mips tests/driver_hello.c
	@$(QEMU_MIPS) ./build/driver/hello_mips; ec=$$?; \
	 [ $$ec -eq 42 ] || { echo "FAIL: driver mips (exit $$ec)"; exit 1; }
	@./build/skjcc -target riscv -S -o build/driver/h.s tests/driver_hello.c; test -s build/driver/h.s \
	 || { echo "FAIL: driver -S produced no asm"; exit 1; }
	@./build/skjcc -target riscv -c -o build/driver/h.o tests/driver_hello.c; test -s build/driver/h.o \
	 || { echo "FAIL: driver -c produced no object"; exit 1; }
	@./build/skjcc -target riscv -o build/driver/two tests/driver_two_a.c tests/driver_two_b.c
	@$(QEMU_RV) ./build/driver/two; ec=$$?; \
	 [ $$ec -eq 42 ] || { echo "FAIL: driver multi-file link (exit $$ec)"; exit 1; }
	@# Hosted link: the driver pulls libc.a by default, so a printf+malloc
	@# program links and runs with no explicit library on the command line.
	@./build/skjcc -target coldfire -o build/driver/host_cf tests/libc_phase_c.c
	@$(QEMU) ./build/driver/host_cf < tests/libc_phase_c.input > build/driver/host_cf.out; ec=$$?; \
	 [ $$ec -eq 42 ] || { echo "FAIL: driver hosted coldfire (exit $$ec)"; exit 1; }
	@diff tests/libc_phase_c.expected build/driver/host_cf.out \
	 || { echo "FAIL: driver hosted coldfire stdout"; exit 1; }
	@./build/skjcc -target riscv -o build/driver/host_rv tests/libc_phase_c.c
	@$(QEMU_RV) ./build/driver/host_rv < tests/libc_phase_c.input > build/driver/host_rv.out; ec=$$?; \
	 [ $$ec -eq 42 ] || { echo "FAIL: driver hosted riscv (exit $$ec)"; exit 1; }
	@diff tests/libc_phase_c.expected build/driver/host_rv.out \
	 || { echo "FAIL: driver hosted riscv stdout"; exit 1; }
	@./build/skjcc -target mips -o build/driver/host_mips tests/libc_phase_c.c
	@$(QEMU_MIPS) ./build/driver/host_mips < tests/libc_phase_c.input > build/driver/host_mips.out; ec=$$?; \
	 [ $$ec -eq 42 ] || { echo "FAIL: driver hosted mips (exit $$ec)"; exit 1; }
	@diff tests/libc_phase_c.expected build/driver/host_mips.out \
	 || { echo "FAIL: driver hosted mips stdout"; exit 1; }
	@# Freestanding: -ffreestanding links only the crt, no libc.  A program
	@# that needs no library still builds and runs.
	@./build/skjcc -target coldfire -ffreestanding -o build/driver/free_cf tests/driver_hello.c
	@$(QEMU) ./build/driver/free_cf; ec=$$?; \
	 [ $$ec -eq 42 ] || { echo "FAIL: driver -ffreestanding coldfire (exit $$ec)"; exit 1; }
	@echo "PASS: cross driver (build+run, -S, -c, multi-file, hosted libc, -ffreestanding)"

## The Phase A C standard library (doc/tools/libc.md): headers, string/ctype, the
## snprintf formatter, and stdlib numeric/sort.  Each target compiles the libc
## sources plus a self-checking test with its skj-cc, links over the crt, and
## runs under qemu (exit 42).  Same per-target env as the check-cc-* suites.
## See tests/run-libc-tests.sh.
check-libc-cf: build/skj-cc build/skj-as build/start.o build/half.o build/va_cf.o build/co_switch_cf.o
	@OUTDIR="$(CURDIR)/build/libc-cf" \
	 HALF="$(CURDIR)/build/half.o $(CURDIR)/build/va_cf.o $(CURDIR)/build/co_switch_cf.o" \
	 FS_INTERPOSE=1 FS_AMBIENT_DEFAULT=1 \
	 sh tests/run-libc-tests.sh

check-libc-rv: build/skj-cc-rv build/rv/start.o build/rv/va_rv.o build/rv/soft64_div.o build/rv/co_switch.o
	@CCBIN="$(CURDIR)/build/skj-cc-rv" \
	 ASM="$(RV_AS) -march=rv32imfd_zfh -mabi=ilp32 -o" \
	 LD="$(RV_LD) -m elf32lriscv -o" \
	 QEMU="$(QEMU_RV)" \
	 START="$(CURDIR)/build/rv/start.o" \
	 HALF="$(CURDIR)/build/rv/va_rv.o $(CURDIR)/build/rv/soft64_div.o $(CURDIR)/build/rv/co_switch.o" \
	 OUTDIR="$(CURDIR)/build/libc-rv" \
	 FS_INTERPOSE=1 FS_AMBIENT_DEFAULT=1 \
	 sh tests/run-libc-tests.sh

check-libc-mips: build/skj-cc-mips build/mips/start.o build/mips/half.o build/mips/va_mips.o build/mips/soft64_div.o build/mips/co_switch.o
	@CCBIN="$(CURDIR)/build/skj-cc-mips" \
	 ASM="$(MIPS_AS) -march=mips1 -EL -o" \
	 LD="$(MIPS_LD) -o" \
	 QEMU="$(QEMU_MIPS)" \
	 START="$(CURDIR)/build/mips/start.o" \
	 HALF="$(CURDIR)/build/mips/half.o $(CURDIR)/build/mips/va_mips.o $(CURDIR)/build/mips/soft64_div.o $(CURDIR)/build/mips/co_switch.o" \
	 OUTDIR="$(CURDIR)/build/libc-mips" \
	 sh tests/run-libc-tests.sh

check-libc-x86-64: build/skj-cc-x86-64 build/x86_64/start_sysv.o build/x86_64/half.o build/x86_64/va.o build/x86_64/soft64.o build/x86_64/co_switch.o
	@CCBIN="$(CURDIR)/build/skj-cc-x86-64" \
	 ASM="$(X64_ASM) $(X64_ASMFLAGS) -o" \
	 LD="$(X64_LD) -m elf_x86_64 -o" \
	 QEMU="$(QEMU_X64)" \
	 START="$(CURDIR)/build/x86_64/start_sysv.o" \
	 HALF="$(CURDIR)/build/x86_64/half.o $(CURDIR)/build/x86_64/va.o $(CURDIR)/build/x86_64/soft64.o $(CURDIR)/build/x86_64/co_switch.o" \
	 OUTDIR="$(CURDIR)/build/libc-x86_64" \
	 sh tests/run-libc-tests.sh

check-libc-arm64: build/skj-cc-arm64 build/arm64/start_aapcs.o build/arm64/va.o build/arm64/soft64.o build/arm64/co_switch.o
	@CCBIN="$(CURDIR)/build/skj-cc-arm64" \
	 ASM="$(A64_AS) -o" \
	 LD="$(A64_LD) -o" \
	 QEMU="$(firstword $(QEMU_A64))" \
	 START="$(CURDIR)/build/arm64/start_aapcs.o" \
	 HALF="$(CURDIR)/build/arm64/va.o $(CURDIR)/build/arm64/soft64.o $(CURDIR)/build/arm64/co_switch.o" \
	 OUTDIR="$(CURDIR)/build/libc-arm64" \
	 sh tests/run-libc-tests.sh

## check-libc aggregates only the targets that pass today: RISC-V, x86-64, and
## arm64 (all using the shared instruction selector).  All five are aggregated
## and passing: the 64-bit-codegen bug the older cf_emit.c/mips_emit.c backends
## once hit on the Phase A printf was fixed, so ColdFire and MIPS run the suite
## too.
check-libc: check-libc-cf check-libc-rv check-libc-mips check-libc-x86-64 check-libc-arm64
	@echo "libc Phase A+B+C+E+F+G+H: cf/rv/mips/x86-64/arm64 passed."

## Stock-gcc interlink: a gcc object built with the default flags (PIE and
## linker relaxation, so R_RISCV_GOT_HI20 and R_RISCV_CALL_PLT) linked into a
## static executable by skj-ld-rv, which synthesizes the GOT.  Proves the
## toolchain interlinks stock gcc objects, not only ones built -fno-pic
## -mno-relax.
test-cc-rv-psabi-gcc-stock: build/skj-cc-rv build/skj-as-rv build/skj-ld-rv build/rv-skj/start_psabi_cc.o | build/rv-skj
	@$(RV_CC) $(RV_RT_CFLAGS) -c -o build/rv-skj/stock_lib.o tests/rv_gcc_stock_lib.c
	@./build/skj-cc-rv -o build/rv-skj/stock_main.s tests/rv_gcc_stock_main.c
	@./build/skj-as-rv -o build/rv-skj/stock_main.o build/rv-skj/stock_main.s
	@./build/skj-ld-rv -o build/rv-skj/stock build/rv-skj/start_psabi_cc.o \
	    build/rv-skj/stock_main.o build/rv-skj/stock_lib.o
	@$(QEMU_RV) ./build/rv-skj/stock; ec=$$?; \
	 if [ $$ec -eq 42 ]; then echo "PASS: cc-rv-psabi stock-gcc interlink (GOT + CALL_PLT, exit 42)"; \
	 else echo "FAIL: cc-rv-psabi stock-gcc interlink (exit $$ec, expected 42)"; exit 1; fi

## Archive (.a) support: skj-ld-rv pulls the members that resolve undefined
## symbols (member A, and B transitively) and skips the unreferenced one (whose
## own undefined symbol would break the link if it were wrongly pulled).
test-cc-rv-psabi-archive: build/skj-cc-rv build/skj-as-rv build/skj-ld-rv build/rv-skj/start_psabi_cc.o | build/rv-skj
	@$(RV_CC) $(RV_RT_CFLAGS) -c -o build/rv-skj/arch_a.o tests/rv_archive_a.c
	@$(RV_CC) $(RV_RT_CFLAGS) -c -o build/rv-skj/arch_b.o tests/rv_archive_b.c
	@$(RV_CC) $(RV_RT_CFLAGS) -c -o build/rv-skj/arch_unused.o tests/rv_archive_unused.c
	@rm -f build/rv-skj/libarch.a
	@$(RV_AR) rcs build/rv-skj/libarch.a build/rv-skj/arch_a.o build/rv-skj/arch_b.o \
	    build/rv-skj/arch_unused.o
	@./build/skj-cc-rv -o build/rv-skj/arch_main.s tests/rv_archive_main.c
	@./build/skj-as-rv -o build/rv-skj/arch_main.o build/rv-skj/arch_main.s
	@./build/skj-ld-rv -o build/rv-skj/arch build/rv-skj/start_psabi_cc.o \
	    build/rv-skj/arch_main.o build/rv-skj/libarch.a
	@$(QEMU_RV) ./build/rv-skj/arch; ec=$$?; \
	 if [ $$ec -eq 42 ]; then echo "PASS: cc-rv-psabi archive link (member pulled from .a, unused skipped, exit 42)"; \
	 else echo "FAIL: cc-rv-psabi archive link (exit $$ec, expected 42)"; exit 1; fi

## C compiler tests on MIPS I (skj-cc-mips, qemu-mipsel).  Stack convention
## like RISC-V, so the psABI-only tests are excluded; half.o supplies
## _Float16 (MIPS I has no hardware half).
check-cc-mips: build/skj-cc-mips build/mips/start.o build/mips/half.o build/mips/va_mips.o
	@CCBIN="$(CURDIR)/build/skj-cc-mips" \
	 ASM="$(MIPS_AS) -march=mips1 -EL -o" \
	 LD="$(MIPS_LD) -o" \
	 QEMU="$(QEMU_MIPS)" \
	 START="$(CURDIR)/build/mips/start.o" \
	 HALF="$(CURDIR)/build/mips/half.o $(CURDIR)/build/mips/va_mips.o" \
	 OUTDIR="$(CURDIR)/build/cc-mips" \
	 EXCLUDE="$(MIPS_EXCLUDE)" \
	 sh tests/run-cc-tests.sh

## skj-as-mips validation: the encoding golden-master against GNU as (the
## .set noreorder enc_*.s fixtures, byte-for-byte), runnable programs under
## qemu-mipsel, and the spim R2000 load-delay tier that catches the hazard
## qemu's interlocked pipeline hides.  Needs mipsel-none-elf-{as,ld,objcopy}
## and qemu-mipsel; the spim tier is skipped if spim is absent.
check-mipsas: build/skj-as-mips build/skj-ld-mips
	@SKJ="$(CURDIR)/build/skj-as-mips" SKJLD="$(CURDIR)/build/skj-ld-mips" \
	 GAS="$(MIPS_AS)" GLD="$(MIPS_LD)" \
	 OBJCOPY="$(MIPS_OBJCOPY)" QEMU="$(QEMU_MIPS)" SPIM="$(SPIM)" \
	 sh tests/run-mipsas-tests.sh

## The C suite assembled by skj-as-mips (default .set reorder output) and
## linked by skj-ld-mips, run under qemu-mipsel.  End-to-end proof of the
## binutils-free MIPS toolchain, including the delay-slot scheduler, on real
## backend output.  The counterpart of check-cc-rv-skj.
check-cc-mips-skj: build/skj-cc-mips build/skj-as-mips build/skj-ld-mips build/mips/start.o build/mips/half.o build/mips/va_mips.o
	@CCBIN="$(CURDIR)/build/skj-cc-mips" \
	 ASM="$(CURDIR)/build/skj-as-mips -o" \
	 LD="$(CURDIR)/build/skj-ld-mips -o" \
	 QEMU="$(QEMU_MIPS)" \
	 START="$(CURDIR)/build/mips/start.o" \
	 HALF="$(CURDIR)/build/mips/half.o $(CURDIR)/build/mips/va_mips.o" \
	 OUTDIR="$(CURDIR)/build/cc-mips-skj" \
	 EXCLUDE="$(MIPS_EXCLUDE)" \
	 sh tests/run-cc-tests.sh

## Archive (.a) support end to end: skj-ar builds a GNU archive with a symbol
## index, and skj-ld-mips pulls the members that resolve undefined symbols
## (member A, and B transitively) while skipping the unreferenced one (whose own
## undefined symbol would break the link if it were wrongly pulled).  Proves
## both the new skj-ar tool and the skj-ld-mips archive reader.  The fixtures
## are the language-neutral archive C shared with the RISC-V archive test.
test-mips-archive: build/skj-cc-mips build/skj-as-mips build/skj-ld-mips build/skj-ar build/mips/start.o build/mips/half.o | build/mips
	@for m in a b unused; do \
	    ./build/skj-cc-mips -o build/mips/arch_$$m.s tests/rv_archive_$$m.c; \
	    ./build/skj-as-mips -o build/mips/arch_$$m.o build/mips/arch_$$m.s; \
	 done
	@rm -f build/mips/libarch.a
	@./build/skj-ar rcs build/mips/libarch.a build/mips/arch_a.o \
	    build/mips/arch_b.o build/mips/arch_unused.o
	@./build/skj-cc-mips -o build/mips/arch_main.s tests/rv_archive_main.c
	@./build/skj-as-mips -o build/mips/arch_main.o build/mips/arch_main.s
	@./build/skj-ld-mips -o build/mips/arch build/mips/start.o build/mips/half.o \
	    build/mips/arch_main.o build/mips/libarch.a
	@$(QEMU_MIPS) ./build/mips/arch; ec=$$?; \
	 if [ $$ec -eq 42 ]; then echo "PASS: mips archive link (skj-ar built, member pulled from .a, unused skipped, exit 42)"; \
	 else echo "FAIL: mips archive link (exit $$ec, expected 42)"; exit 1; fi

## Extended inline asm on MIPS: skj-cc's asm(template : outputs : inputs :
## clobbers).  mips_asm_ext exercises generic "r"/"=r"/"+r" operands (each
## placed in an allocated $s-register or a pool $t-register), mips_asm_syscall
## exercises register pinning through GNU local register variables (operands
## bound to $v0/$a0..$a2 for a real write(2) syscall).  Each is built and run
## under both the default scheduler and the .set noreorder one, since the
## user's instructions must survive delay-slot scheduling untouched.  The
## binaries go in build/mips-asm/, not build/mips/, so the generic MIPS test
## runner (which auto-discovers build/mips/<name> against tests/<name>.*) does
## not pick them up and run them with the wrong expected exit code.
test-cc-mips-asm: build/skj-cc-mips build/skj-as-mips build/skj-ld-mips build/mips/start.o build/mips/half.o | build/mips-asm
	@for t in "mips_asm_ext 42" "mips_asm_syscall 22"; do \
	    set -- $$t; name=$$1; want=$$2; \
	    for sched in reorder noreorder; do \
	        if [ $$sched = noreorder ]; then nr=SKJ_MIPS_NOREORDER=1; else nr=; fi; \
	        env $$nr ./build/skj-cc-mips -o build/mips-asm/$$name.s tests/$$name.c || exit 1; \
	        ./build/skj-as-mips -o build/mips-asm/$$name.o build/mips-asm/$$name.s || exit 1; \
	        ./build/skj-ld-mips -o build/mips-asm/$$name build/mips/start.o \
	            build/mips/half.o build/mips-asm/$$name.o || exit 1; \
	        $(QEMU_MIPS) ./build/mips-asm/$$name; ec=$$?; \
	        if [ $$ec -eq $$want ]; then \
	            echo "PASS: $$name ($$sched, exit $$ec)"; \
	        else \
	            echo "FAIL: $$name ($$sched, exit $$ec, expected $$want)"; exit 1; \
	        fi; \
	    done; \
	 done

## Extended inline asm on ColdFire: skj-cc's asm(template : outputs : inputs :
## clobbers) through skj-as/skj-ld on qemu-m68k.  Generic "r"/"=r"/"+r"
## operands and register pinning through GNU local register variables (pins on
## the non-allocatable scratch %d0/%d1).  Built in build/cf-asm/ so the generic
## ColdFire test runner does not pick it up with the wrong expected exit code.
test-cc-cf-asm: build/skj-cc build/skj-as build/skj-ld build/start.o build/half.o | build/cf-asm
	@for t in "cf_asm_ext 42" "cf_asm_syscall 22"; do \
	    set -- $$t; name=$$1; want=$$2; \
	    ./build/skj-cc -o build/cf-asm/$$name.s tests/$$name.c || exit 1; \
	    ./build/skj-as -o build/cf-asm/$$name.o build/cf-asm/$$name.s || exit 1; \
	    ./build/skj-ld -o build/cf-asm/$$name build/start.o build/half.o \
	        build/cf-asm/$$name.o || exit 1; \
	    $(QEMU) ./build/cf-asm/$$name; ec=$$?; \
	    if [ $$ec -eq $$want ]; then echo "PASS: $$name (exit $$ec)"; \
	    else echo "FAIL: $$name (exit $$ec, expected $$want)"; exit 1; fi; \
	 done

## Extended inline asm on RISC-V: skj-cc-rv through skj-as-rv/skj-ld-rv on
## qemu-riscv32.  Generic "r"/"=r"/"+r" operands and register pinning through
## GNU local register variables, exercised by a real write(2) ecall (a7/a0..a2
## pinned).  Built in build/rv-asm/.
test-cc-rv-asm: build/skj-cc-rv build/skj-as-rv build/skj-ld-rv build/rv/start.o | build/rv-asm
	@./build/skj-cc-rv -o build/rv-asm/rv_asm_ext.s tests/rv_asm_ext.c
	@./build/skj-as-rv -o build/rv-asm/rv_asm_ext.o build/rv-asm/rv_asm_ext.s
	@./build/skj-ld-rv -o build/rv-asm/rv_asm_ext build/rv/start.o \
	    build/rv-asm/rv_asm_ext.o
	@$(QEMU_RV) ./build/rv-asm/rv_asm_ext; ec=$$?; \
	 if [ $$ec -eq 42 ]; then echo "PASS: rv_asm_ext (exit $$ec)"; \
	 else echo "FAIL: rv_asm_ext (exit $$ec, expected 42)"; exit 1; fi

## Extended inline asm on x86-64: skj-cc-x86-64 through as/ld on qemu-x86_64.
## Generic "r"/"=r"/"+r" operands (names at the operand width, eax vs rax) and
## register pinning through GNU local register variables, exercised by a real
## write(2) syscall (rax/rdi/rsi/rdx pinned).  Built in build/x86-asm/.
test-cc-x86-64-asm: build/skj-cc-x86-64 build/x86_64/start_sysv.o build/x86_64/half.o build/x86_64/va.o | build/x86-asm
	@for t in "x86_asm_ext 42" "x86_asm_syscall 22"; do \
	    set -- $$t; name=$$1; want=$$2; \
	    ./build/skj-cc-x86-64 -o build/x86-asm/$$name.s tests/$$name.c || exit 1; \
	    $(X64_ASM) $(X64_ASMFLAGS) -o build/x86-asm/$$name.o build/x86-asm/$$name.s || exit 1; \
	    $(X64_LD) -m elf_x86_64 -o build/x86-asm/$$name \
	        build/x86_64/start_sysv.o build/x86_64/half.o build/x86_64/va.o \
	        build/x86-asm/$$name.o || exit 1; \
	    $(QEMU_X64) ./build/x86-asm/$$name; ec=$$?; \
	    if [ $$ec -eq $$want ]; then echo "PASS: $$name (exit $$ec)"; \
	    else echo "FAIL: $$name (exit $$ec, expected $$want)"; exit 1; fi; \
	 done

## Extended inline asm on AArch64: skj-cc-arm64 through the GNU assembler/linker
## on qemu-aarch64.  Generic "r"/"=r"/"+r" operands (names at the operand width,
## w vs x) and register pinning through GNU local register variables, exercised
## by a real write(2) svc (x8/x0/x1/x2 pinned).  Built in build/arm64-asm/.
test-cc-arm64-asm: build/skj-cc-arm64 build/arm64/start_aapcs.o build/arm64/va.o | build/arm64-asm
	@./build/skj-cc-arm64 -o build/arm64-asm/arm64_asm_ext.s tests/arm64_asm_ext.c
	@$(A64_AS) -o build/arm64-asm/arm64_asm_ext.o build/arm64-asm/arm64_asm_ext.s
	@$(A64_LD) -o build/arm64-asm/arm64_asm_ext \
	    build/arm64/start_aapcs.o build/arm64/va.o build/arm64-asm/arm64_asm_ext.o
	@$(QEMU_A64) ./build/arm64-asm/arm64_asm_ext; ec=$$?; \
	 if [ $$ec -eq 42 ]; then echo "PASS: arm64_asm_ext (exit $$ec)"; \
	 else echo "FAIL: arm64_asm_ext (exit $$ec, expected 42)"; exit 1; fi

## PlayStation PS-EXE output: skj-ld-mips -f ps-exe writes the "PS-X EXE"
## container the console loads, based at the PlayStation RAM address 0x80010000,
## with the BIOS-TTY crt (start_psx.o).  No in-tree simulator loads a PS-EXE and
## models the R3051, so this validates the container structurally (header +
## flat image) and confirms the payload was relocated at the PlayStation base;
## the code itself is run-verified at the Linux base by check-cc-mips-skj.  A
## few C programs and a hand-written assembly program are packed and checked.
test-mips-psexe: build/skj-cc-mips build/skj-as-mips build/skj-ld-mips build/mips/start_psx.o build/mips/half.o | build/mips
	@for t in cc_t001_return cc_t040_func_ptr cc_t069_calls; do \
	    ./build/skj-cc-mips -o build/mips/psx_$$t.s tests/$$t.c || exit 1; \
	    ./build/skj-as-mips -o build/mips/psx_$$t.o build/mips/psx_$$t.s || exit 1; \
	    ./build/skj-ld-mips -f ps-exe -o build/mips/psx_$$t.exe \
	        build/mips/start_psx.o build/mips/half.o build/mips/psx_$$t.o || exit 1; \
	    OBJDUMP="$(MIPS_OBJDUMP)" sh tests/run-psx-tests.sh build/mips/psx_$$t.exe || exit 1; \
	 done

## PS-EXE functional tier: actually run the PlayStation executables on the
## PCSX-Redux emulator with a real BIOS ROM.  The BIOS-TTY crt's exit path
## writes main's return value to the PCSX-Redux software-exit register, so in
## test mode the emulator quits with that code and the run self-checks like the
## qemu tiers.  Opt-in: needs pcsx-redux and a BIOS, skipped otherwise.  Pass
## the BIOS with PSX_BIOS=/path/to/scphXXXX.bin.
## Two phases.  Integer programs run natively on the R3051 (skj-cc-mips).  A
## float program traps on the FPU-less R3051, so it goes through the soft-float
## compiler (skj-cc-mips-sf, no cop-1) linked with softfloat.o: that is the
## milestone-6 goal, float code running on the real console, demonstrated here.
PCSX_REDUX ?= pcsx-redux
## A real PlayStation BIOS is not distributable, so no path is baked in.
## RetroArch keeps its BIOS set in ~/.config/retroarch/system, so default to a
## PS1 BIOS found there (scph5501 US first, then the other PS1 ROMs); fall back
## to empty when none is present, which makes the runner skip.  Override
## PSX_BIOS to point elsewhere.
PSX_BIOS ?= $(firstword $(wildcard \
    $(HOME)/.config/retroarch/system/scph5501.bin \
    $(HOME)/.config/retroarch/system/scph5500.bin \
    $(HOME)/.config/retroarch/system/scph5502.bin \
    $(HOME)/.config/retroarch/system/scph5552.bin))
test-mips-psexe-redux: build/skj-cc-mips build/skj-cc-mips-sf build/skj-as-mips build/skj-ld-mips build/mips/start_psx.o build/mips/half.o build/mips/softfloat.o | build/mips
	@echo "== integer programs (native R3051) =="
	@CCBIN="$(CURDIR)/build/skj-cc-mips" \
	 ASM="$(CURDIR)/build/skj-as-mips -o" \
	 LD="$(CURDIR)/build/skj-ld-mips -f ps-exe -o" \
	 START="$(CURDIR)/build/mips/start_psx.o" \
	 HALF="$(CURDIR)/build/mips/half.o" \
	 OUTDIR="$(CURDIR)/build/psx-redux" \
	 PCSX_REDUX="$(PCSX_REDUX)" PSX_BIOS="$(PSX_BIOS)" \
	 sh tests/run-psx-redux-tests.sh \
	    cc_t001_return cc_t002_arith cc_t003_if cc_t015_ternary \
	    cc_t040_func_ptr cc_t017_bitwise
	@echo "== float programs, soft-float (FPU-less R3051) =="
	@CCBIN="$(CURDIR)/build/skj-cc-mips-sf" \
	 ASM="$(CURDIR)/build/skj-as-mips -o" \
	 LD="$(CURDIR)/build/skj-ld-mips -f ps-exe -o" \
	 START="$(CURDIR)/build/mips/start_psx.o" \
	 HALF="$(CURDIR)/build/mips/half.o" \
	 EXTRA="$(CURDIR)/build/mips/softfloat.o" \
	 OUTDIR="$(CURDIR)/build/psx-redux-sf" \
	 PCSX_REDUX="$(PCSX_REDUX)" PSX_BIOS="$(PSX_BIOS)" \
	 sh tests/run-psx-redux-tests.sh cc_t069_calls

## The .set noreorder tier: the same programs, but the backend fills the
## delay slots itself (SKJ_MIPS_NOREORDER=1) instead of the assembler.  This
## verifies the scheduler functionally: a wrong hoist changes results and
## fails here even though qemu cannot see a raw R2000 load-delay hazard.
## A separate output tree so the reorder binaries are untouched.
build/mips-nr/%.s: tests/%.tc build/skj-tinc-mips | build/mips-nr
	SKJ_MIPS_NOREORDER=1 ./build/skj-tinc-mips -o $@ $<

build/mips-nr/scm_%.s: tests/scm_%.scm build/skj-sc-mips | build/mips-nr
	SKJ_MIPS_NOREORDER=1 ./build/skj-sc-mips -o $@ $<

build/mips-nr/%.o: build/mips-nr/%.s
	$(MIPS_AS) -march=mips1 -EL -o $@ $<

build/mips-nr/start.o: runtime/start_mips.S | build/mips-nr
	$(MIPS_AS) -march=mips1 -EL -o $@ $<

build/mips-nr/%: build/mips-nr/%.o build/mips-nr/start.o
	$(MIPS_LD) -o $@ build/mips-nr/start.o $<

build/mips-nr:
	mkdir -p build/mips-nr

build/mips-asm:
	mkdir -p build/mips-asm

build/cf-asm:
	mkdir -p build/cf-asm

build/rv-asm:
	mkdir -p build/rv-asm

build/x86-asm:
	mkdir -p build/x86-asm

build/arm64-asm:
	mkdir -p build/arm64-asm

check-mips-noreorder: build/skj-tinc-mips build/skj-sc-mips $(MIPS_TESTS:tests/%.tc=build/mips-nr/%) $(MIPS_SCM_TESTS:tests/%.scm=build/mips-nr/%)
	@sh tests/run-tests.sh "$(QEMU_MIPS)" build/mips-nr

## The C compiler suite under the noreorder scheduler (int, float, i64,
## _Float16 coverage).  Same tools, SKJ_MIPS_NOREORDER exported.
check-cc-mips-noreorder: build/skj-cc-mips build/mips/start.o build/mips/half.o build/mips/va_mips.o
	@SKJ_MIPS_NOREORDER=1 CCBIN="$(CURDIR)/build/skj-cc-mips" \
	 ASM="$(MIPS_AS) -march=mips1 -EL -o" \
	 LD="$(MIPS_LD) -o" \
	 QEMU="$(QEMU_MIPS)" \
	 START="$(CURDIR)/build/mips/start.o" \
	 HALF="$(CURDIR)/build/mips/half.o $(CURDIR)/build/mips/va_mips.o" \
	 OUTDIR="$(CURDIR)/build/cc-mips-nr" \
	 EXCLUDE="$(MIPS_EXCLUDE)" \
	 sh tests/run-cc-tests.sh

## The C compiler suite compiled soft-float: skj-cc-mips-sf emits no cop-1
## instruction and the programs link softfloat.o beside half.o.  The float
## tests (t054/t057/t060/t070) exercise the soft-float path end to end.
check-cc-mips-sf: build/skj-cc-mips-sf build/mips/start.o build/mips/half.o build/mips/softfloat.o
	@CCBIN="$(CURDIR)/build/skj-cc-mips-sf" \
	 ASM="$(MIPS_AS) -march=mips1 -EL -o" \
	 LD="$(MIPS_LD) -o" \
	 QEMU="$(QEMU_MIPS)" \
	 START="$(CURDIR)/build/mips/start.o" \
	 HALF="$(CURDIR)/build/mips/half.o $(CURDIR)/build/mips/softfloat.o" \
	 OUTDIR="$(CURDIR)/build/cc-mips-sf" \
	 EXCLUDE="$(MIPS_EXCLUDE) $(VA_STDARG_EXCLUDE)" \
	 sh tests/run-cc-tests.sh

## The whole PlayStation soft-float tier.
check-mips-sf: test-softfloat test-fpu-mips-sf test-f32-mips-sf check-cc-mips-sf

## The whole MIPS tier in one target, the counterpart to check-emu-all, so CI
## and a MIPS working session have a single entry point.  Everything under
## qemu-mipsel with the GNU mipsel binutils: the front-end suites and the three
## IR-builder integration tests, the C suite (reorder and
## noreorder), the binutils-free toolchain (skj-as-mips / skj-ld-mips / skj-ar,
## the encoding golden-master, the archive and PS-EXE paths), and the FPU-less
## soft-float configuration.  test-mips-psexe-redux is excluded (it needs a
## real PlayStation BIOS and is opt-in).
check-mips-all: check-mips check-mips-noreorder \
                test-i64-mips test-ops-mips test-fpu-mips test-f32-mips \
                check-cc-mips check-cc-mips-noreorder \
                check-mipsas check-cc-mips-skj test-mips-archive test-mips-psexe \
                test-cc-mips-asm check-mips-sf test-threaded-mips
	@echo "All MIPS test suites passed."

check-cpp: build/skj-cpp
	@sh tests/run-cpp-tests.sh

## Smoke test: full in-tree toolchain (front end -> skj-as -> skj-ld -> qemu)
check-smoke: build/skj-tinc build/skj-sc build/skj-mooc build/skj-cc build/skj-as build/skj-ld
	@sh tests/smoke.sh "$(QEMU)"

## skj-as validation: assemble everything with skj-as instead of m68k-linux-gnu-as
check-as: build/skj-tinc build/skj-sc build/skj-mooc build/skj-pc build/skj-as
	@sh tests/check-as.sh "$(QEMU)"

## RISC-V RV32 assembler + linker: encoding golden-master against the GNU
## assembler, plus self-checking programs run on both skj-run and qemu-riscv32
## (needs riscv64-linux-gnu-{as,ld,objcopy} and qemu-riscv32).
check-rvas: build/skj-as-rv build/skj-ld-rv build/skj-run
	@SKJ=./build/skj-as-rv SKJLD=./build/skj-ld-rv RUN=./build/skj-run \
	 GAS="$(RV_AS)" GLD="$(RV_LD)" QEMU="$(QEMU_RV)" \
	 sh tests/run-rvas-tests.sh

## x86-64 assembler: encoding + relocation golden-master against GNU as in
## intel mode.  Linking stays external, so there are no run tests here; the
## x86-64 cc/libc/pc suites exercise running through the assembler when
## X64_ASM points at it.
check-x86-64as: build/skj-as-x86-64
	@SKJ=./build/skj-as-x86-64 GAS=as GASFLAGS=--64 \
	 sh tests/run-x86-64as-tests.sh

## AArch64 assembler: encoding + relocation golden-master against GNU as.  As
## with the x86-64 lane, linking stays external so there are no run tests here;
## the arm64 cc/libc/pc suites exercise running through the assembler when
## A64_AS points at it.
check-aarch64as: build/skj-as-aarch64
	@SKJ=./build/skj-as-aarch64 GAS=aarch64-linux-gnu-as \
	 sh tests/run-aarch64as-tests.sh

## Full qemu matrix across every supported architecture: the per-language
## suites, plus the IR-builder integration tests (fpu / i64 / unsigned ops).
## ColdFire (qemu-m68k), RISC-V (qemu-riscv32, hardware float), x86-64
## (qemu-x86_64), AArch64 (qemu-aarch64).
## Every skj-* tool recognizes -h / --help: prints its usage and exits 0.  One
## representative per distinct entry point (the cross variants share a main.c
## with the host tool, and skj-kpc covers the kpcom seed).  This keeps the -h
## surface uniform as tools are added.
HELP_TOOLS := skj-tinc skj-sc skj-mooc skj-pc skj-cc skj-cpp skj-jit \
              skj-as skj-as-rv skj-as-mips skj-as-x86-64 skj-as-aarch64 skj-ld skj-ld-rv skj-ld-mips skj-ar \
              skj-run skj-addr2line skj-dbg skj-kp skj-kp-jit skj-kp-patrol \
              skj-kpc
test-help: $(addprefix build/,$(HELP_TOOLS))
	@ok=1; \
	 for t in $(HELP_TOOLS); do \
	     out=`./build/$$t -h 2>&1`; rc=$$?; \
	     if [ $$rc != 0 ] || ! printf '%s' "$$out" | grep -qi usage; then \
	         echo "FAIL: $$t -h (rc=$$rc)"; ok=0; \
	     fi; \
	 done; \
	 if [ $$ok = 1 ]; then \
	     echo "PASS: -h prints usage and exits 0 on all $(words $(HELP_TOOLS)) tools"; \
	 else exit 1; fi

check-all: check check-cc check-cc-x86-64 check-cc-arm64 check-cc-rv check-cpp \
           test-cc-cf-asm test-cc-rv-asm test-cc-x86-64-asm test-cc-arm64-asm \
           check-rv check-x86-64 check-arm64 \
           check-x86-oracle check-arm64-oracle check-rv-oracle check-jit check-jit-arm64 \
           check-jit-rv check-jit-arm64-disasm check-jit-rv-disasm \
           test-jit-trap test-jit-trap-arm64 test-jit-arm64-high \
           test-jit-arena test-fs \
           test-gc test-coro test-threads \
           test-fpu test-fpu-rv test-fpu-x86-64 test-fpu-arm64 \
           test-f32-diff \
           test-i64 test-i64-rv test-i64-x86-64 test-i64-arm64 \
           test-ops test-ops-rv test-ops-x86-64 test-ops-arm64 \
           test-threaded test-threaded-rv test-threaded-x86-64 test-threaded-arm64 \
           check-rvas check-x86-64as check-aarch64as check-cc-rv-skj \
           test-cc-rv-loc test-kp-rv-loc test-kp-rv-dwarf test-kp-rv-trap-backtrace test-pc-loc test-moo-loc test-tinc-loc \
           test-dwarf-line test-rv-stt-func test-rv-debug-sections \
           test-rv-debug-link test-rv-addr2line test-rv-backtrace test-rv-abort-backtrace \
           test-bt-loop test-bt-trim test-rv-wild-backtrace \
           test-cf-addr2line test-mips-addr2line \
           test-cf-wild-backtrace test-cf-abort-backtrace \
           test-kp-cf-dwarf test-kp-cf-trap-backtrace \
           test-rv-dbg test-rv-gdb test-rv-sandbox-io test-rv-libc-io test-rv-stat test-rv-fileops test-rv-dirs test-cf-stat test-cf-fileops test-cf-dirs test-rv-fs-cap test-cf-fs-cap check-emu-posix test-rv-arc4random test-cf-arc4random test-rv-selfhost test-cf-selfhost test-mips-selfhost test-rv-selfhost-toolchain test-cf-selfhost-toolchain test-rv-selfhost-kpc test-cf-selfhost-kpc check-selfhost-all test-help \
           check-cc-rv-psabi test-cc-rv-psabi-interop \
           test-cc-rv-psabi-gcc-stock test-cc-rv-psabi-archive \
           test-cc-rv-psabi-struct test-cc-driver check-libc \
           check-kp \
           check-kp-frontend check-kp-jit check-kp-aot check-kp-asm check-kp-corpus check-kp-corpus-cf check-kp-multi check-kp-files check-kp-dirs check-kp-stdlib-free check-kp-stdlib-free-cf check-kp-io check-kp-inc
	@echo "All architecture test suites passed."

clean:
	rm -rf build

## Code-generator coverage (V.D): build the codegen files --coverage across a
## matrix of front end / backend configurations, compile each corpus, and
## report per-file line coverage of ir/, the backend regalloc/selector/emitter
## pairs, and the text sinks.  Pure host build (every tool emits assembly, so no
## cross toolchain or qemu is needed), self-contained.
coverage-codegen:
	@tests/codegen-coverage.sh

## Echo a Makefile variable: `make print-CC_SRC`.  Used by codegen-coverage.sh
## to reuse the source lists here instead of duplicating them.
print-%:
	@echo '$($*)'

.PHONY: help check-pc-host check-pc-arm64 all release-check check check-rv run-rv test-rv-irq test-rv-run test-guest-mmap test-rv-sandbox test-rv-xlang-sandbox test-rv-decode-cache test-rv-expand test-rv-bus test-rv-csr test-rv-decode test-rv-fp check-archtest check-rv32 test-rv32-apps test-rv32-unit test-rv32-program test-rv32-lockstep test-rv32-fuzz audit-rv32-coverage audit-rv32-icov audit-rv32-mutants check-emu check-rv-emu check-emu-all check-emu-san bench bench-cf bench-rv bench-accept test-fpu-emu test-i64-emu test-ops-emu check-x86-64 check-arm64 check-x86-oracle check-arm64-oracle check-rv-oracle check-jit check-jit-arm64 check-jit-rv check-jit-arm64-disasm check-jit-rv-disasm test-jit-trap test-jit-trap-arm64 test-jit-arm64-high check-all check-slow check-cc check-cc-x86-64 check-cc-arm64 check-cc-rv check-cpp check-as check-rvas check-x86-64as check-aarch64as check-cc-rv-skj check-cc-rv-psabi test-cc-rv-psabi-interop test-cc-rv-psabi-gcc-stock test-cc-rv-psabi-archive test-cc-rv-psabi-struct test-cc-driver check-libc check-libc-cf check-libc-mips check-libc-rv check-libc-x86-64 check-libc-arm64 check-mipsas check-cc-mips-skj test-cc-mips-asm test-cc-cf-asm test-cc-rv-asm test-cc-x86-64-asm test-cc-arm64-asm check-mips-all check-smoke test-gc test-coro test-threads test-jit-arena test-fs check-fs-free test-fpu test-fpu-rv test-fpu-x86-64 test-fpu-arm64 test-f32-rv f32-oracle f32-golden-check test-f32-diff test-f32-diff-rv test-f32-diff-x86-64 test-f32-diff-arm64 test-f32-diff-mips test-f32-diff-cf test-torture-diff test-i64 test-i64-rv test-i64-x86-64 test-i64-arm64 test-ops test-ops-rv test-ops-x86-64 test-ops-arm64 test-threaded test-threaded-rv test-threaded-x86-64 test-threaded-arm64 test-threaded-mips test-parse bootstrap-fpc check-kp-frontend check-kp-jit check-kp-aot check-kp-asm check-kp-jit-rv check-kp-jit-arm64 check-kp-aot-rv check-kp-aot-arm64 check-kp-free check-kp-free-cf check-kp-corpus check-kp-corpus-san check-kp-corpus-rv check-kp-corpus-arm64 check-kp-corpus-cf check-kp-multi check-kp-multi-rv check-kp-multi-arm64 check-kp-files check-kp-dirs check-kp-stdlib-free check-kp-stdlib-free-cf check-kp-io check-kp-io-rv check-kp-io-arm64 check-kp-io-free check-kp-inc check-kp-inc-rv check-kp-inc-arm64 check-kp-inc-free test-help test-rv-sandbox-io test-rv-libc-io test-rv-stat test-rv-fileops test-rv-dirs test-cf-stat test-cf-fileops test-cf-dirs test-rv-fs-cap test-cf-fs-cap check-emu-posix test-rv-arc4random test-cf-arc4random test-rv-selfhost test-rv-selfhost-full test-cf-selfhost test-cf-selfhost-full test-mips-selfhost test-rv-selfhost-toolchain test-cf-selfhost-toolchain test-rv-selfhost-kpc test-cf-selfhost-kpc test-rv-selfhost-kpc-full test-cf-selfhost-kpc-full check-selfhost-all test-rv-abort-backtrace test-bt-loop test-bt-trim test-rv-wild-backtrace test-kp-rv-dwarf test-kp-rv-trap-backtrace test-cf-addr2line test-mips-addr2line test-cf-wild-backtrace test-cf-abort-backtrace test-kp-cf-dwarf test-kp-cf-trap-backtrace test-rng test-rng-glue test-sched-glue coverage-codegen clean FORCE
