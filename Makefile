# Makefile : build the Skjegg compiler tools and their test programs.
#
# Host build:         make          -> build/skj-sc, build/skj-mooc,
#                                      build/skj-pc, build/skj-cc
# Target build+run:   make check    -> assembles/links each tests/*.scm
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
# Host architecture, for choosing native over cross tools.  On an arm64 host
# (e.g. an Apple Silicon Mac or an arm64 Linux box) the AArch64 target is native,
# so its C compiler is the host cc, not a cross gcc, and the native in-process
# JIT is the arm64 one (jit_arm64 + call_guest_arm64), never the x86 one.
HOST_ARCH := $(shell uname -m)
HOST_IS_ARM64 := $(filter arm64 aarch64,$(HOST_ARCH))
# The host OS, to pick OS-specific backends (e.g. the VMM: Darwin uses
# Hypervisor.framework, Linux uses KVM).
HOST_OS := $(shell uname -s)
HOST_IS_DARWIN := $(filter Darwin,$(HOST_OS))
# The in-process JIT that is native to this host: the arm64 one on an arm64 host,
# the x86 one otherwise.  Building the x86 JIT on an arm64 host (or vice versa)
# cannot work, its byte sink and call trampoline are for the other ISA.
NATIVE_JIT := $(if $(HOST_IS_ARM64),build/skj-jit-arm64,build/skj-jit)
# The in-process Kobold JIT native to this host, same reasoning as NATIVE_JIT.
NATIVE_KP_JIT := $(if $(HOST_IS_ARM64),build/skj-kp-jit-arm64,build/skj-kp-jit)
# The ahead-of-time Kobold compiler and its runtime archive native to this host.
# The runtime bundles host-arch assembly (co_switch/call_guest), so the pair is
# host-arch-bound exactly like the JIT.
NATIVE_KP       := $(if $(HOST_IS_ARM64),build/skj-kpc-arm64,build/skj-kpc)
NATIVE_LIBKPAOT := $(if $(HOST_IS_ARM64),build/libkpaot-arm64.a,build/libkpaot.a)

A64_AS  ?= build/skj-as-aarch64
A64_LD  ?= aarch64-linux-gnu-ld
# Host-tool link flag: the kpcom front ends are linked -no-pie on Linux (the AOT
# runtime's low-memory model expects code and data below 4GB).  macOS is PIE-only
# and clang rejects -no-pie, so the flag is dropped on a Darwin host.
HOST_NO_PIE := $(if $(HOST_IS_DARWIN),,-no-pie)
A64_CC  ?= $(if $(HOST_IS_ARM64),cc,aarch64-linux-gnu-gcc)
# Rules that run $(A64_AS) depend on $(A64_ASM_DEP) so the in-tree assembler is
# built first and dependents re-assemble when it changes; an override to the
# host as clears the dependency.
ifeq ($(A64_AS),build/skj-as-aarch64)
A64_ASM_DEP := build/skj-as-aarch64
else
A64_ASM_DEP :=
endif
QEMU_A64 ?= qemu-aarch64
# How to run an arm64 binary on this host: directly on an arm64 host (an Apple
# Silicon Mac or an arm64 Linux box, where the in-process JIT and native AOT
# output run without emulation), through qemu-aarch64 on any other host.  Used
# by the in-process JIT test lanes, whose driver binary is host-native.
A64_RUN := $(if $(HOST_IS_ARM64),,$(QEMU_A64))
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
# skj-cc-arm64 uses this; the stack-ABI arm64 scheme tool
# and the i64/ops/fpu integration tests keep BE_A64 (arm64_emit.c), the same
# split x86 has between BE_X64_SEL and BE_X64.
BE_A64_SEL := backend/regalloc_arm64.c backend/arm64_select.c backend/arm64_mc_text.c
# The unified x86-64 (LP64) backend: the shared instruction selector over the
# GNU intel-syntax text sink, driven by the AOT driver.  skj-cc-x86-64 uses this; the other
# x86-64 tools still use BE_X64 (x86_emit.c) until M3 moves them over.
BE_X64_SEL := backend/regalloc_x86.c backend/x86_select.c backend/mc_text.c \
              backend/x86_aot.c
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

TOOLS := build/skj-sc build/skj-sc-rv build/skj-sc-mips build/skj-sc-arm64 \
         build/skj-mooc build/skj-pc build/skj-as build/skj-ld \
         build/skj-as-rv build/skj-ld-rv build/skj-as-mips build/skj-ld-mips build/skj-ar \
         build/skj-as-x86-64 build/skj-as-aarch64 build/skj-ld-aarch64 \
         build/skj-ld-x86-64 build/skj-ld-arm64 \
         build/skj-cpp build/skj-cc build/skj-cc-x86-64 build/skj-cc-arm64 \
         build/skj-cc-rv build/skjcc build/skj-run \
         build/skj-addr2line build/skj-dbg \
         $(NATIVE_JIT) $(NATIVE_KP_JIT) $(if $(HOST_IS_ARM64),,build/skj-kp-patrol) \
         $(NATIVE_KP) $(NATIVE_LIBKPAOT) build/skj-kpc \
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

# Enabled once for the whole Makefile: it lets a pattern rule's prerequisites
# reference the stem through $$(...) so the per-arch source list stays a real
# prerequisite (rebuild on source change), not just a recipe argument.
.SECONDEXPANSION:

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

build/skj-cc: $(CC_SRC) cc/cc.h cpp/cpp.h cpp/internal.h ir/ir.h ir/exepath.h | build
	$(CC) $(CFLAGS) -DCC_M68K $(SKJ_INC_DEF) -Icc -Icpp -Iir -o $@ $(CC_SRC)

# The cross C compilers, keyed by the tool-name suffix so one pattern rule
# builds them all.  cc_src_<suffix> aliases the CC_SRC_* source lists (which
# other rules also use); cc_flags_<suffix> is the target's defines and include
# path.  arm64-macho is the arm64 cc fixed to the Darwin/Mach-O target
# (-DSKJ_DARWIN), so Mach-O output can be generated and checked on a non-Darwin
# host.  mips-sf is the PlayStation (R3051, no cop-1) soft-float build:
# -DMIPS_SOFTFLOAT emits no cop-1 instruction and lowers every float op to a
# runtime/softfloat.c call.  The backend headers are a wildcard prerequisite so
# any of them triggers a rebuild of the relevant tools.
cc_src_x86-64      := $(CC_SRC_X64)
cc_src_arm64       := $(CC_SRC_A64)
cc_src_arm64-macho := $(CC_SRC_A64)
cc_src_rv          := $(CC_SRC_RV)
cc_src_mips        := $(CC_SRC_MIPS)
cc_src_mips-sf     := $(CC_SRC_MIPS)
cc_flags_x86-64      := -DX86_BITS=64 -DSKJ_LP64=1 -DCC_PSABI -DCC_STRUCT_ABI $(SKJ_INC_DEF) -Icc -Icpp -Iir -Ibackend
cc_flags_arm64       := -DSKJ_LP64=1 -DCC_PSABI -DCC_ARM64 $(SKJ_INC_DEF) -Icc -Icpp -Iir
cc_flags_arm64-macho := -DSKJ_LP64=1 -DCC_PSABI -DCC_ARM64 -DSKJ_DARWIN $(SKJ_INC_DEF) -Icc -Icpp -Iir
cc_flags_rv          := -DCC_RISCV $(SKJ_INC_DEF) -Icc -Icpp -Iir
cc_flags_mips        := -DCC_MIPS $(SKJ_INC_DEF) -Icc -Icpp -Iir
cc_flags_mips-sf     := -DCC_MIPS -DMIPS_SOFTFLOAT $(SKJ_INC_DEF) -Icc -Icpp -Iir

build/skj-cc-%: $$(cc_src_$$*) cc/cc.h cpp/cpp.h cpp/internal.h ir/ir.h ir/exepath.h $(wildcard backend/*.h) | build
	$(CC) $(CFLAGS) $(cc_flags_$*) -o $@ $(cc_src_$*)

## skjcc: the cross driver.  One command runs the per-target cc -> as -> ld
## pipeline and links the crt + runtime helpers from <libdir>/<target>/.  Lib
## dir resolution: the SKJ_CC_LIB env var wins; else, for a relocated install,
## <bindir>/../lib/skjegg beside the driver when present; else the compiled-in
## path baked here (the in-tree staging tree, build/lib).  Sub-tools are found
## beside skjcc.
build/skjcc: driver/main.c ir/exepath.h build/version.h | build
	$(CC) $(CFLAGS) -Iir -DSKJ_LIB_DIR='"$(CURDIR)/build/lib"' -o $@ driver/main.c

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
# Both hosted Kobold tiers reach the host through the shared fs/ layer (hostfs),
# not raw libc, so their file/dir primitives (kp_rt_io.c) use the one directory
# and fd model the sandbox uses.  These sources and flags ride every JIT-family
# link and every hosted AOT archive below.  (The freestanding archives use
# io_free.c, which already routes through fs/ under SKJ_FS_INTERPOSE.)
KP_FS       := fs/fs.c fs/hostfs.c
KP_FS_FLAGS := -DFS_WITH_HOSTFS -Ifs

KP_RT  := kobold/kpm.c kobold/str_core.c kobold/str_fmt.c kobold/dynarr.c kobold/map.c kobold/rt_host.c \
          kobold/co_shim.c runtime/co_switch_x86.S kobold/kp_host.c kobold/kp_io.c kobold/kp_rt_io.c kobold/kp_rt_meta.c libc/numfmt.c \
          runtime/rng.c runtime/rng_glue.c runtime/sched_glue.c $(KP_FS) \
          jit/jit_common.c jit/jit_arena.c jit/jit_x86.c jit/emit_x86.c jit/code.c \
          jit/call_guest_x86.S \
          backend/x86_select.c backend/regalloc_x86.c $(IR_SRC)
KP_HDRS := kobold/kpm.h kobold/str.h kobold/dynarr.h kobold/map.h kobold/rt_glue.h kobold/obj.h kobold/gheap_core.h kobold/co.h kobold/kp_rt_io.h \
           kobold/handle.h kobold/lowmem.h kobold/kp_host.h jit/jit_common.h jit/jit_arena.h fs/fs.h fs/hostfs.h \
           jit/jit_x86.h jit/emit_x86.h jit/code.h backend/mc.h backend/x86_select.h ir/ir.h ir/target.h runtime/rng.h
# -DKP_GLUE_HOSTED: the hosted Kobold runtime (the in-process JIT and libkpaot.a)
# links runtime/rng_glue.c and sched_glue.c against the host libc clock, sleep,
# and entropy, not the freestanding 32-bit syscall seam those files use on rv/cf.
KP_CFLAGS := $(CFLAGS) -DX86_BITS=64 -DKP_FREESTANDING -DKP_REAL=double -DKP_GLUE_HOSTED -Iir -Ijit -Ibackend -Ikobold

## The native Kobold front end linked into the in-process JIT: kpcom (compiled by
## skj-pc, the same object skj-kpc uses) plus the host Pascal runtime, the C module
## builder, the KPM writer, and the AOT emit helper kpb_fe references.  This
## replaces the embedded seed and the JIT-of-the-compiler: kpcom runs as linked
## native code and compiles straight into the builder (kp_compile_pas calls its
## CompileModule export and serializes kpb_module_ptr() with kpm_write).  kpcom's
## compile-time file I/O is the kpc_* host shim in pascal_kpcom_host.c, named apart
## from the JIT runtime's own ReadFileStr so the two coexist in one binary.  kpm.c
## is already in KP_RT, so it is not repeated here.
KP_FE_SRC := tests/pascal_kpcom_host.c kobold/kpbuild.c kobold/kpb_fe.c kobold/kpm_write.c
KP_FE_HDRS := kobold/kpbuild.h

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
## The seed is the single-step kpcom (built -D KP_ONESTEP), so a JIT that embeds
## it drives the C module builder through the bound kpbp_* and carries no text
## emitter.  -D KP_TARGET_<x> stamps the matching .target tag / pointer width.
build/kpcom-x86-64.kpm: build/skj-kpc $(KPCOM_SRCS) Makefile | build
	build/skj-kpc -D KP_ONESTEP -D KP_TARGET_X64 --emit-kpm $(KPCOM_SRC_PAS) > $@
build/kpcom-arm64.kpm: build/skj-kpc-arm64 $(KPCOM_SRCS) Makefile | build
	build/skj-kpc-arm64 -D KP_ONESTEP -D KP_TARGET_ARM64 --emit-kpm $(KPCOM_SRC_PAS) > $@
build/kpcom-rv.kpm: build/skj-kpc-rv $(KPCOM_SRCS) Makefile | build
	build/skj-kpc-rv -D KP_ONESTEP -D KP_TARGET_RV --emit-kpm $(KPCOM_SRC_PAS) > $@
build/kpcom-cf.kpm: build/skj-kpc-cf $(KPCOM_SRCS) Makefile | build
	build/skj-kpc-cf -D KP_ONESTEP -D KP_TARGET_CF --emit-kpm $(KPCOM_SRC_PAS) > $@

## bin2c embeds the seed per architecture (same symbol and header name; each JIT
## compiles with its own -Ibuild/<arch> ahead of -Ibuild so frontend.c's
## `#include "kpcom_kpm.h"` picks up the matching seed).
build/x86-64/kpcom_kpm.c: build/kpcom-x86-64.kpm build/bin2c | build
	@mkdir -p build/x86-64
	build/bin2c kpcom_kpm build/kpcom-x86-64.kpm build/x86-64/kpcom_kpm.c build/x86-64/kpcom_kpm.h
build/arm64/kpcom_kpm.c: build/kpcom-arm64.kpm build/bin2c | build
	@mkdir -p build/arm64
	build/bin2c kpcom_kpm build/kpcom-arm64.kpm build/arm64/kpcom_kpm.c build/arm64/kpcom_kpm.h

## kpcom-fe-jit.o: the skj-pc-built kpcom object with its program entry `main`
## renamed aside, so it links beside jit_main.c's own main.  kpcom's main never
## runs in the JIT (kp_compile_pas calls CompileModule directly, which sets the
## target itself), so the renamed entry is simply unreferenced.  build/skj-kpc
## leaves build/kpcom-x86-64-fe.o beside itself; depend on skj-kpc to keep it
## fresh, then rename its main.
build/x86-64/kpcom-fe-jit.o: build/skj-kpc | build
	@mkdir -p build/x86-64
	objcopy --redefine-sym main=kpcom_prog_main build/kpcom-x86-64-fe.o $@

## skj-kp-jit: the native (x86-64, LP64) in-process JIT.  Runtime and front end
## compiled for x86-64 (-DX86_BITS=64 in KP_CFLAGS) so guest pointers are full
## 64-bit; kp_co is the LP64 coroutine object.  No seed: kpcom is linked in as
## native code (kpcom-fe-jit.o + the host Pascal runtime + the C builder) and
## compiles straight into the builder in one step.
build/skj-kp-jit: kobold/jit_main.c kobold/frontend.c build/x86-64/kpcom-fe-jit.o build/pascal_rt_host.o $(KP_FE_SRC) $(KP_RT) build/kp_co_lp64.o kobold/frontend.h $(KP_HDRS) $(KP_FE_HDRS) Makefile | build
	$(CC) $(KP_CFLAGS) $(KP_FS_FLAGS) $(HOST_NO_PIE) -DKP_FE_LINKED -Ibuild -o $@ kobold/jit_main.c kobold/frontend.c build/x86-64/kpcom-fe-jit.o build/pascal_rt_host.o $(KP_FE_SRC) $(KP_RT) build/kp_co_lp64.o -lm

## kp_co_lp64.o: the coroutine core compiled LP64 (-DX86_BITS=64: native 64-bit
## guest pointer/handle, guest stacks in full memory), linked into the x86-64
## LP64 tools.
build/kp_co_lp64.o: kobold/co.c kobold/co.h kobold/handle.h kobold/lowmem.h kobold/obj.h Makefile | build
	$(CC) -std=c99 -O2 -Wall -Wextra -Wno-unused-parameter -DX86_BITS=64 -Iir -Ijit -Ibackend -Ikobold -c kobold/co.c -o $@

## Host-native codegen for building the host front-end tools (skj-kpc*).  These
## tools embed the kpcom front end as native machine code, so they must be built
## for THIS host's architecture to run, exactly like the native JIT.  HOST_PC is
## the Pascal compiler that emits host-native assembly and host_assemble runs the
## matching in-tree assembler; the x86-64 (X64) and AArch64 (A64) assemblers take
## their operands in different orders, so the call is wrapped.  The TARGET each
## tool emits modules for is set by its -d KP_TARGET_* flag and is independent of
## the host.
ifeq ($(HOST_IS_ARM64),)
HOST_PC      := build/skj-pc-x86-64
HOST_ASM_DEP := $(X64_ASM_DEP)
host_assemble = $(X64_ASM) $(X64_ASMFLAGS) $(1) -o $(2)
else
HOST_PC      := build/skj-pc-arm64
HOST_ASM_DEP := $(A64_ASM_DEP)
host_assemble = $(A64_AS) -o $(2) $(1)
endif

## skj-kpc: the native single-step Kobold compiler, the host compiler for the
## x86-64 target.  Plan 9 style, the target baked at build: skj-pc compiles
## kpcom.pas with -d KP_TARGET_X64, so CurTarget is x86-64 (8-byte pointers, width
## derived from the target) and every module it emits is stamped `.target x86-64`.
## -d KP_ONESTEP links the C module builder, the AOT emit pipeline, and the x86-64
## backend into the front-end binary, so kpcom drives the builder directly
## (EmitModuleViaBuilder) and emits in one process: .kp -> assembly with -o out.s,
## or the serialized KPM image with --emit-kpm (through kpm_write, for a seed or a
## stored unit), no KPM text round trip.  It links the host-import shim
## (ReadFileStr, WriteFileStr, kpd_*) and skj-pc's Pascal runtime, the same ones
## check-pc-kpcom uses.  skj-kpc-x86-64 is the explicit arch name for the same
## binary.  Built host-native (HOST_PC/host_assemble) so it runs on this host
## whatever its architecture.
build/skj-kpc: $(HOST_PC) tests/pascal_kpcom_host.c build/pascal_rt_host.o $(KPCOM_SRCS) \
               kobold/kpbuild.c kobold/kpb_fe.c kobold/kpb_native.c kobold/kp_aot.c kobold/kpm.c \
               kobold/kpm_write.c $(IR_SRC) $(BE_X64_SEL) \
               kobold/kpbuild.h kobold/kpm.h kobold/kp_aot.h $(HOST_ASM_DEP) | build
	$(HOST_PC) -d KOBOLD -d KP_TARGET_X64 -d KP_ONESTEP -o build/kpcom-x86-64-fe.s $(KPCOM_SRC_PAS)
	$(call host_assemble,build/kpcom-x86-64-fe.s,build/kpcom-x86-64-fe.o)
	$(CC) $(CFLAGS) $(HOST_NO_PIE) -DX86_BITS=64 -Iir -Ibackend -Ikobold \
	    build/kpcom-x86-64-fe.o tests/pascal_kpcom_host.c build/pascal_rt_host.o \
	    kobold/kpbuild.c kobold/kpb_fe.c kobold/kpb_native.c kobold/kp_aot.c kobold/kpm.c kobold/kpm_write.c \
	    $(IR_SRC) $(BE_X64_SEL) -lm -o $@

## skj-kpc-arm64: the host front end for the AArch64 target.  Same LP64 width as
## skj-kpc, but KP_TARGET_ARM64 stamps `.target arm64`, so the arm64 backend accepts
## its modules and rejects the x86-64 ones.  The arch it names is the target it
## emits for, not where it runs; it is built host-native like skj-kpc.
build/skj-kpc-arm64: $(HOST_PC) tests/pascal_kpcom_host.c build/pascal_rt_host.o $(KPCOM_SRCS) \
                     kobold/kpbuild.c kobold/kpb_fe.c kobold/kpb_native.c kobold/kp_aot.c kobold/kpm.c \
                     kobold/kpm_write.c $(IR_SRC) $(BE_A64_SEL) \
                     kobold/kpbuild.h kobold/kpm.h kobold/kp_aot.h $(HOST_ASM_DEP) | build
	$(HOST_PC) -d KOBOLD -d KP_TARGET_ARM64 -d KP_ONESTEP -o build/kpcom-arm64-fe.s $(KPCOM_SRC_PAS)
	$(call host_assemble,build/kpcom-arm64-fe.s,build/kpcom-arm64-fe.o)
	$(CC) $(CFLAGS) $(HOST_NO_PIE) -DCC_ARM64 -Iir -Ibackend -Ikobold \
	    build/kpcom-arm64-fe.o tests/pascal_kpcom_host.c build/pascal_rt_host.o \
	    kobold/kpbuild.c kobold/kpb_fe.c kobold/kpb_native.c kobold/kp_aot.c kobold/kpm.c kobold/kpm_write.c \
	    $(IR_SRC) $(BE_A64_SEL) -lm -o $@

build/skj-kpc-x86-64: build/skj-kpc | build
	cp -f build/skj-kpc $@

## check-onestep: the single-step equivalence gate.  skj-kpc emits in one step
## (.kp -> .s); that assembly must byte-match the assembly its serialized module
## reaches through the backend (skj-kpc --emit-kpm | skj-kpc), so the direct
## kp_aot_emit path and the kpm_write + kpm_read round trip agree.  The reference
## and the single-step tool are now the same binary.
check-onestep: build/skj-kpc build/skj-kpc
	@KPC=build/skj-kpc KP=build/skj-kpc ONESTEP=build/skj-kpc \
	    sh tests/run-onestep.sh $(KPM_RT_SRCS)


## skj-kpc-rv / skj-kpc-cf: the per-target host front ends for the rv32 and
## ColdFire backends.  Each is skj-pc compiling kpcom.pas with its target baked in
## (KP_TARGET_RV / KP_TARGET_CF), so the module it emits is stamped `.target rv32`
## or `.target cf` and is bound to that one architecture.  One compiler per target,
## the cc model: a unit built for rv32 is not a unit for ColdFire, so there is no
## shared "ilp32" front end.  (The architecture decides the pointer width; both
## happen to be 4 bytes, but that is a consequence, not a shared category.)
## Each per-target skj-kpc is single-step like the x86-64 one: -d KP_ONESTEP plus
## the C module builder, the AOT emit pipeline, and that target's backend linked
## into the host-native front-end binary.  The C sources carry the same arch
## flags the linked backend needs (the FE object is host-native;
## only the baked target and the linked backend change per arch).
build/skj-kpc-rv: $(HOST_PC) tests/pascal_kpcom_host.c build/pascal_rt_host.o $(KPCOM_SRCS) \
                  kobold/kpbuild.c kobold/kpb_fe.c kobold/kpb_native.c kobold/kp_aot.c kobold/kpm.c \
                  kobold/kpm_write.c $(IR_SRC) $(BE_RV) \
                  kobold/kpbuild.h kobold/kpm.h kobold/kp_aot.h $(HOST_ASM_DEP) | build
	$(HOST_PC) -d KOBOLD -d KP_TARGET_RV -d KP_ONESTEP -o build/kpcom-rv-fe.s $(KPCOM_SRC_PAS)
	$(call host_assemble,build/kpcom-rv-fe.s,build/kpcom-rv-fe.o)
	$(CC) $(CFLAGS) $(HOST_NO_PIE) -DKP_AOT_RV -DSKJ_KPM_ARCH='"rv32"' -Iir -Ibackend -Ikobold \
	    build/kpcom-rv-fe.o tests/pascal_kpcom_host.c build/pascal_rt_host.o \
	    kobold/kpbuild.c kobold/kpb_fe.c kobold/kpb_native.c kobold/kp_aot.c kobold/kpm.c kobold/kpm_write.c \
	    $(IR_SRC) $(BE_RV) -lm -o $@

build/skj-kpc-cf: $(HOST_PC) tests/pascal_kpcom_host.c build/pascal_rt_host.o $(KPCOM_SRCS) \
                  kobold/kpbuild.c kobold/kpb_fe.c kobold/kpb_native.c kobold/kp_aot.c kobold/kpm.c \
                  kobold/kpm_write.c $(IR_SRC) $(BE_CF) \
                  kobold/kpbuild.h kobold/kpm.h kobold/kp_aot.h $(HOST_ASM_DEP) | build
	$(HOST_PC) -d KOBOLD -d KP_TARGET_CF -d KP_ONESTEP -o build/kpcom-cf-fe.s $(KPCOM_SRC_PAS)
	$(call host_assemble,build/kpcom-cf-fe.s,build/kpcom-cf-fe.o)
	$(CC) $(CFLAGS) $(HOST_NO_PIE) -DSKJ_KPM_ARCH='"cf"' -Iir -Ibackend -Ikobold \
	    build/kpcom-cf-fe.o tests/pascal_kpcom_host.c build/pascal_rt_host.o \
	    kobold/kpbuild.c kobold/kpb_fe.c kobold/kpb_native.c kobold/kp_aot.c kobold/kpm.c kobold/kpm_write.c \
	    $(IR_SRC) $(BE_CF) -lm -o $@

## skj-kpc-mips: the single-step Kobold compiler for the MIPS / PlayStation
## target.  HOST_PC compiles kpcom.pas with KP_TARGET_MIPS, so the modules it
## emits stamp `.target mips` (ILP32) and bind to the MIPS backend, which is built
## soft-float (the PlayStation R3051 has no FPU).
build/skj-kpc-mips: $(HOST_PC) tests/pascal_kpcom_host.c build/pascal_rt_host.o $(KPCOM_SRCS) \
                    kobold/kpbuild.c kobold/kpb_fe.c kobold/kpb_native.c kobold/kp_aot.c kobold/kpm.c \
                    kobold/kpm_write.c $(IR_SRC) $(BE_MIPS) \
                    kobold/kpbuild.h kobold/kpm.h kobold/kp_aot.h $(HOST_ASM_DEP) | build
	$(HOST_PC) -d KOBOLD -d KP_TARGET_MIPS -d KP_ONESTEP -o build/kpcom-mips-fe.s $(KPCOM_SRC_PAS)
	$(call host_assemble,build/kpcom-mips-fe.s,build/kpcom-mips-fe.o)
	$(CC) $(CFLAGS) $(HOST_NO_PIE) -DMIPS_SOFTFLOAT -DSKJ_KPM_ARCH='"mips"' -Iir -Ibackend -Ikobold \
	    build/kpcom-mips-fe.o tests/pascal_kpcom_host.c build/pascal_rt_host.o \
	    kobold/kpbuild.c kobold/kpb_fe.c kobold/kpb_native.c kobold/kp_aot.c kobold/kpm.c kobold/kpm_write.c \
	    $(IR_SRC) $(BE_MIPS) -lm -o $@

## skj-kp-jit-san: build/skj-kp-jit with the guest-heap write-after-free instrument
## (GH_POISON, kobold/rt_host.c) plus ASan/UBSan, at -O1 -g so a report carries a usable
## backtrace.  This is the tool that validates the string/array/map runtimes' reference
## counting: an over-release, double free, or use-after-free in an in-place mutator (for
## example the string self-append path, kp_str_append*) trips here even when the program's
## printed values happen to look right.  Kept out of `all`; built on demand by
## check-kp-corpus-san.
## It is the native (LP64) JIT, so a width-8 truncation (a 64-bit guest address
## narrowed to 32 bits dereferencing wild) also trips here under ASan.
## The sanitizer JIT keeps the seed path: it validates the run module's kp-runtime
## heap (GH_POISON + ASan), and it instruments that best by JIT'ing kpcom as
## kp-runtime code, where ASan can see it.  The linked native kpcom uses the
## malloc-based host Pascal runtime that ASan does not instrument, so it stays on
## the seed here until the runtime unification lets the san build link it cleanly.
build/skj-kp-jit-san: kobold/jit_main.c kobold/frontend.c build/x86-64/kpcom_kpm.c kobold/kpbuild.c kobold/kpm_write.c $(KP_RT) build/kp_co_lp64.o kobold/frontend.h kobold/kpbuild.h $(KP_HDRS) Makefile | build
	$(CC) $(KP_CFLAGS) $(KP_FS_FLAGS) -Ibuild/x86-64 -O1 -g -DGH_POISON $(SAN_CFLAGS) -Ibuild -o $@ kobold/jit_main.c kobold/frontend.c build/x86-64/kpcom_kpm.c kobold/kpbuild.c kobold/kpm_write.c $(KP_RT) build/kp_co_lp64.o -lm

build/skj-kp-patrol: kobold/patrol_main.c $(KP_RT) build/kp_co.o $(KP_HDRS) Makefile | build
	$(CC) $(KP_CFLAGS) $(KP_FS_FLAGS) -o $@ kobold/patrol_main.c $(KP_RT) build/kp_co.o -lm

## skj-kp-jit-rv: the in-process RV32 JIT for Kobold modules, the cross-arch
## counterpart of skj-kp-jit.  Same driver, ported runtime, and selector; only the
## arch-specific byte sink (jit_rv/emit_rv), the stack-ABI i64 helpers, the call
## trampoline, and the coroutine switch differ.  rt_host/co_shim need no change: on
## a 32-bit host a plain mmap is low by construction, so the guest address model
## holds without MAP_32BIT.  Built with RVJIT_CC (zig's bundled rv32 libc, since the
## rv64 toolchain has no ilp32 multilib) and run under qemu-riscv32.
KP_RT_RV := kobold/kpm.c kobold/str_core.c kobold/str_fmt.c kobold/dynarr.c kobold/map.c kobold/rt_host.c \
            kobold/co_shim.c runtime/co_switch_rv.S kobold/kp_host.c kobold/kp_io.c kobold/kp_rt_io.c kobold/kp_rt_meta.c libc/numfmt.c \
            runtime/rng.c runtime/rng_glue.c runtime/sched_glue.c $(KP_FS) \
            jit/jit_common.c jit/jit_arena.c jit/jit_rv.c jit/emit_rv.c jit/code.c \
            jit/call_guest_rv.S jit/rv_jit_i64.S \
            backend/rv_select.c backend/regalloc_rv.c $(IR_SRC)
KP_HDRS_RV := kobold/kpm.h kobold/str.h kobold/dynarr.h kobold/map.h kobold/rt_glue.h kobold/obj.h kobold/gheap_core.h kobold/co.h kobold/kp_rt_io.h \
              kobold/handle.h kobold/lowmem.h kobold/kp_host.h jit/jit_common.h jit/jit_arena.h fs/fs.h fs/hostfs.h \
              jit/emit_rv.h jit/code.h backend/rv_mc.h backend/rv_select.h ir/ir.h ir/target.h
KP_CFLAGS_RV := $(CFLAGS) -DKP_FREESTANDING -DKP_REAL=double -Iir -Ijit -Ibackend -Ikobold

## co.c built for rv with the same vendored-warning set as the x86 kp_co.o.
build/kp_co_rv.o: kobold/co.c kobold/co.h kobold/handle.h kobold/lowmem.h Makefile | build
	$(RVJIT_CC) -std=c99 -O2 -Wall -Wextra -Wno-unused-parameter -Iir -Ijit -Ibackend -Ikobold -c kobold/co.c -o $@

build/skj-kp-jit-rv: kobold/jit_main.c kobold/frontend.c build/kpcom_kpm.c kobold/kpbuild.c kobold/kpm_write.c $(KP_RT_RV) build/kp_co_rv.o kobold/frontend.h kobold/kpbuild.h $(KP_HDRS_RV) runtime/i64_rv.S Makefile | build
	$(RVJIT_CC) $(KP_CFLAGS_RV) $(KP_FS_FLAGS) -DSKJ_KPM_ARCH='"rv32"' -Ibuild -o $@ kobold/jit_main.c kobold/frontend.c build/kpcom_kpm.c kobold/kpbuild.c kobold/kpm_write.c $(KP_RT_RV) build/kp_co_rv.o -lm

## skj-kp-jit-arm64: the in-process AArch64 JIT for Kobold modules, the cross-arch
## counterpart of skj-kp-jit.  Same driver, ported runtime, and selector, over the
## AArch64 byte sink (jit_arm64/emit_arm64), call trampoline, and coroutine switch.
## i64 is native (no stack-ABI helpers) and floats ride the AAPCS64 d-registers, so
## no ABI bridge is needed, unlike rv32.  On this LP64 host a guest address must
## still be below 4GB, so the guest heap and coroutine stacks are mapped low (see
## rt_host.c/co_shim.c).  Cross-built with the arm64 toolchain and run under
## qemu-aarch64, which executes the JIT-generated code.
KP_RT_A64 := kobold/kpm.c kobold/str_core.c kobold/str_fmt.c kobold/dynarr.c kobold/map.c kobold/rt_host.c \
             kobold/co_shim.c runtime/co_switch_arm64.S kobold/kp_host.c kobold/kp_io.c kobold/kp_rt_io.c kobold/kp_rt_meta.c libc/numfmt.c \
             runtime/rng.c runtime/rng_glue.c runtime/sched_glue.c $(KP_FS) \
             jit/jit_common.c jit/jit_arena.c jit/jit_arm64.c jit/emit_arm64.c jit/code.c \
             jit/call_guest_arm64.S \
             backend/arm64_select.c backend/regalloc_arm64.c $(IR_SRC)
KP_HDRS_A64 := kobold/kpm.h kobold/str.h kobold/dynarr.h kobold/map.h kobold/rt_glue.h kobold/obj.h kobold/gheap_core.h kobold/co.h kobold/kp_rt_io.h \
               kobold/handle.h kobold/lowmem.h kobold/kp_host.h jit/jit_common.h jit/jit_arena.h fs/fs.h fs/hostfs.h \
               jit/emit_arm64.h jit/code.h backend/arm64_mc.h backend/arm64_select.h ir/ir.h
KP_CFLAGS_A64 := $(CFLAGS) -DKP_FREESTANDING -DKP_REAL=double -DKP_GLUE_HOSTED -Iir -Ijit -Ibackend -Ikobold

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
build/skj-kp-jit-arm64: kobold/jit_main.c kobold/frontend.c build/arm64/kpcom_kpm.c kobold/kpbuild.c kobold/kpm_write.c $(KP_RT_A64) build/kp_co_a64_lp64.o kobold/frontend.h kobold/kpbuild.h $(KP_HDRS_A64) Makefile | build
	$(A64_CC) $(KP_CFLAGS_A64) $(KP_FS_FLAGS) -DCC_ARM64 -Ibuild/arm64 -Ibuild -o $@ kobold/jit_main.c kobold/frontend.c build/arm64/kpcom_kpm.c kobold/kpbuild.c kobold/kpm_write.c $(KP_RT_A64) build/kp_co_a64_lp64.o -lm

## Exercise the KPM1 module reader's input validation (kobold/kpm.c) through the
## single-step skj-kpc's .kpm-to-assembly path.  Self-contained: no external Kobold
## repo, no qemu, no assembler or linker.  The cases live in tests/kp/ and follow
## the .experror convention (a malformed module must be rejected with the pinned
## message; a well-formed one must compile).
check-kp: build/skj-kpc
	@KP=build/skj-kpc sh tests/run-kp-tests.sh

## skj-kpm-roundtrip: a reader/writer check tool.  It reads a KPM1 module and
## writes it back out through kpm_write (the inverse of kpm_read), so the test
## can confirm the pair is faithful by diffing the assembly skj-kpc emits for the
## original and the re-serialized module.  Links the reader, the writer, and the
## IR core only (no backend).  Built -DX86_BITS=64, matching skj-kpc, so it reads
## the x86-64 modules skj-kpc emits with -D KP_TARGET_X64.
build/skj-kpm-roundtrip: tests/kpm_roundtrip.c kobold/kpm.c kobold/kpm_write.c $(IR_SRC) \
                         kobold/kpm.h kobold/kp_aot.h ir/ir.h Makefile | build
	$(CC) $(CFLAGS) -DX86_BITS=64 -Iir -Ibackend -Ikobold -o $@ \
	    tests/kpm_roundtrip.c kobold/kpm.c kobold/kpm_write.c $(IR_SRC)

## check-kpm-roundtrip: the KPM reader/writer round-trip gate.  For each source
## module, skj-kpc's assembly for the original module must byte-match its assembly
## for the module after a kpm_read -> kpm_write round trip, and a second round
## trip must change nothing (a fixpoint).  Backend-free proof that kpm_write
## preserves everything the backend reads.
KPM_RT_SRCS := tests/kp-corpus/arrays.pas tests/kp-corpus/casts.pas \
               tests/kp-corpus/records.pas tests/kp-corpus/conds.pas \
               tests/kp-corpus/combo.pas tests/kp-corpus/reals.pas \
               tests/kp-corpus/mathfn.pas tests/kp-corpus/sets.pas \
               tests/kp-corpus/strappend.pas tests/kp-corpus/mapbasic.pas \
               tests/kp-corpus/dynarray.pas tests/kp-corpus/strconv.pas
check-kpm-roundtrip: build/skj-kpm-roundtrip build/skj-kpc build/skj-kpc
	@KPC=build/skj-kpc KP=build/skj-kpc RT=build/skj-kpm-roundtrip \
	    sh tests/run-kpm-roundtrip.sh $(KPM_RT_SRCS)

## skj-kpbuild-selftest: exercise the in-memory module builder (kpbuild.c) end
## to end without the front end.  It reads a .kpm, replays it through the kpb_*
## entry points, and checks kp_aot_emit's assembly for the rebuilt module matches
## the assembly for the module read straight from the file.  Links the reader,
## builder, emit pipeline, and the x86-64 selector backend.
build/skj-kpbuild-selftest: tests/kpbuild_selftest.c kobold/kpbuild.c kobold/kpm.c \
                            kobold/kp_aot.c $(IR_SRC) $(BE_X64_SEL) \
                            kobold/kpbuild.h kobold/kpm.h kobold/kp_aot.h ir/ir.h Makefile | build
	$(CC) $(CFLAGS) -DX86_BITS=64 -Iir -Ibackend -Ikobold -o $@ \
	    tests/kpbuild_selftest.c kobold/kpbuild.c kobold/kpm.c kobold/kp_aot.c \
	    $(IR_SRC) $(BE_X64_SEL)

## check-kpbuild: the module-builder gate.  For each corpus module, the assembly
## kp_aot_emit produces after a read -> replay-through-the-builder rebuild must
## byte-match the assembly for the module read straight from the file.  Proves
## the builder carries everything the backend reads, before the front-end cutover.
check-kpbuild: build/skj-kpbuild-selftest build/skj-kpc
	@KPC=build/skj-kpc SELF=build/skj-kpbuild-selftest \
	    sh tests/run-kpbuild.sh $(KPM_RT_SRCS)

## libkpaot.a: the Kobold runtime an AOT program links against.  It is the same
## ported runtime the JIT binds (strings, arrays, maps, coroutines, ARC, the guest
## heap, console I/O) compiled in the freestanding tier with f64 reals, plus the C
## start file kobold/aot_rt.c (process entry, low execution stack, traps, and the
## kprun-compatible debug surface) and kp_call_guest.  Built by name resolution at
## link rather than through a binding table.
## co.c is vendored and builds with -Wall -Wextra but not -Wpedantic, so it is taken
## from the shared build/kp_co.o rule rather than recompiled here.
KP_AOT_RT := kobold/aot_rt.c kobold/kp_rt_io.c kobold/kp_rt_meta.c kobold/kp_host.c kobold/kp_io.c libc/numfmt.c kobold/str_core.c kobold/str_fmt.c kobold/dynarr.c \
             kobold/map.c kobold/rt_host.c kobold/co_shim.c $(KP_FS) \
             runtime/rng.c runtime/rng_glue.c runtime/sched_glue.c \
             runtime/co_switch_x86.S jit/call_guest_x86.S
build/libkpaot.a: $(KP_AOT_RT) build/kp_co_lp64.o $(KP_HDRS) Makefile | build
	@rm -rf build/aot && mkdir -p build/aot
	@for f in $(KP_AOT_RT); do \
	    o=build/aot/`basename $$f | sed 's/\.[cS]$$/.o/'`; \
	    echo "  CC $$f"; \
	    $(CC) $(KP_CFLAGS) $(KP_FS_FLAGS) -c $$f -o $$o || exit 1; \
	done
	@rm -f $@
	$(AR) rcs $@ build/aot/*.o build/kp_co_lp64.o

## libkpaot-rv.a / libkpaot-arm64.a: the target runtimes an AOT program links
## against, the cross-compiled siblings of libkpaot.a.  Same sources (the ported
## runtime plus the aot_rt.c start file) built with the target toolchain and its
## coroutine switch and call trampoline.  rv also carries jit/rv_jit_i64.S, whose
## stack-ABI 64-bit multiply and shifts the guest reaches through the skj_jit_*
## names (the register-ABI __divdi3 family and all FP math come from the toolchain
## runtime library at link).  arm64 needs neither: i64 and doubles are native.
KP_AOT_RT_RV := kobold/aot_rt.c kobold/kp_rt_io.c kobold/kp_rt_meta.c kobold/kp_host.c kobold/kp_io.c libc/numfmt.c kobold/str_core.c kobold/str_fmt.c kobold/dynarr.c \
                kobold/map.c kobold/rt_host.c kobold/co_shim.c $(KP_FS) \
                runtime/co_switch_rv.S jit/call_guest_rv.S jit/rv_jit_i64.S \
                kobold/rv_i64_aot_alias.S
build/libkpaot-rv.a: $(KP_AOT_RT_RV) build/kp_co_rv.o $(KP_HDRS_RV) runtime/i64_rv.S Makefile | build
	@rm -rf build/aot-rv && mkdir -p build/aot-rv
	@for f in $(KP_AOT_RT_RV); do \
	    o=build/aot-rv/`basename $$f | sed 's/\.[cS]$$/.o/'`; \
	    echo "  RVCC $$f"; \
	    $(RVJIT_CC) $(KP_CFLAGS_RV) $(KP_FS_FLAGS) -c $$f -o $$o || exit 1; \
	done
	@rm -f $@
	$(AR) rcs $@ build/aot-rv/*.o build/kp_co_rv.o

KP_AOT_RT_A64 := kobold/aot_rt.c kobold/kp_rt_io.c kobold/kp_rt_meta.c kobold/kp_host.c kobold/kp_io.c libc/numfmt.c kobold/str_core.c kobold/str_fmt.c kobold/dynarr.c \
                 kobold/map.c kobold/rt_host.c kobold/co_shim.c $(KP_FS) \
                 runtime/rng.c runtime/rng_glue.c runtime/sched_glue.c \
                 runtime/co_switch_arm64.S jit/call_guest_arm64.S
build/libkpaot-arm64.a: $(KP_AOT_RT_A64) build/kp_co_a64_lp64.o $(KP_HDRS_A64) Makefile | build
	@rm -rf build/aot-arm64 && mkdir -p build/aot-arm64
	@for f in $(KP_AOT_RT_A64); do \
	    o=build/aot-arm64/`basename $$f | sed 's/\.[cS]$$/.o/'`; \
	    echo "  A64CC $$f"; \
	    $(A64_CC) $(KP_CFLAGS_A64) $(KP_FS_FLAGS) -DCC_ARM64 -c $$f -o $$o || exit 1; \
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
KP_AOT_RT_CF := kobold/aot_rt.c kobold/kp_rt_io.c kobold/kp_rt_meta.c kobold/kp_host.c kobold/kp_io.c libc/numfmt.c kobold/str_core.c kobold/str_fmt.c kobold/dynarr.c \
                kobold/map.c kobold/rt_host.c kobold/co_shim.c $(KP_FS) \
                runtime/co_switch_cf.S jit/call_guest_cf.S
build/libkpaot-cf.a: $(KP_AOT_RT_CF) build/kp_co_cf.o $(KP_HDRS) Makefile | build
	@rm -rf build/aot-cf && mkdir -p build/aot-cf
	@for f in $(KP_AOT_RT_CF); do \
	    o=build/aot-cf/`basename $$f | sed 's/\.[cS]$$/.o/'`; \
	    echo "  CFCC $$f"; \
	    $(M68K_CC) $(KP_CFLAGS_CF) $(KP_FS_FLAGS) -c $$f -o $$o || exit 1; \
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

## Dialect-conformance check: kpcom.pas is the self-hosting compiler, so it must
## stay within the Pascal subset common to skj-pc and a mainstream Pascal compiler
## (fpc), and not drift into a private skj-pc dialect.  Compile it under BOTH and
## require both to accept it.  skj-pc compiles it to assembly (the same step the
## normal build runs); fpc compiles only (-Cn, no link), since the single-step
## externals (kpbp_*) resolve against C objects the fpc build does not link, and
## the check is about the dialect, not a runnable fpc binary.  This is not a seed
## comparison: there is no committed or byte-compared seed (the single-step
## compiler serializes any seed through the C builder, which fpc cannot drive).
FPC ?= fpc
bootstrap-fpc: $(HOST_PC) $(KPCOM_SRCS) $(HOST_ASM_DEP) | build
	@command -v $(FPC) >/dev/null 2>&1 || { echo "bootstrap-fpc: '$(FPC)' required" >&2; exit 1; }
	$(HOST_PC) -d KOBOLD -d KP_TARGET_RV -d KP_ONESTEP -o build/kpcom-dialect.s $(KPCOM_SRC_PAS)
	@echo "bootstrap-fpc: skj-pc accepts kobold/kpcom.pas"
	$(FPC) -dKP_TARGET_RV -dKP_ONESTEP -Cn -Fikobold -FEbuild -FUbuild -O2 -Xs -veiwn kobold/kpcom.pas
	@echo "bootstrap-fpc: fpc accepts kobold/kpcom.pas (both compilers agree on the dialect)"

## ---- Kobold source fixtures (tests/kp-src/*.pas + *.expected) ----
## The source-compilation counterpart of check-kp (which exercises the KPM1
## reader on pre-built modules).  run-kp-src-tests.sh runs each fixture in one of
## three modes; see its header.  The x86 targets go in check-all, the rv/arm64
## ones in check-emu-all (they run under qemu).

# Per-arch environments for run-kp-src-tests.sh.  The AOT path (host front end
# emits a module, the target backend compiles and links it, the result runs) and
# the freestanding path (skj assembler + linker over the no-libc crt) each take
# the same block of variables at every call site; only KP_SRCDIR varies by
# corpus.  Each recipe sets KP_SRCDIR (when not the default kp-src) and the
# fragment, then runs the script.
KP_AOT_ENV_x86_64 = MODE=aot FRONTEND=build/skj-kpc KPAOT=build/skj-kpc KPAOTLIB=build/libkpaot.a KPAOTCC=$(CC) KPAOTAS="$(X64_ASM) $(X64_ASMFLAGS)"
KP_AOT_ENV_rv = MODE=aot FRONTEND=build/skj-kpc-rv KPAOT=build/skj-kpc-rv KPAOTLIB=build/libkpaot-rv.a KPAOTCC="$(RVJIT_CC)" KPAOTQEMU="$(QEMU_RV)"
KP_AOT_ENV_arm64 = MODE=aot FRONTEND=build/skj-kpc-arm64 KPAOT=build/skj-kpc-arm64 KPAOTLIB=build/libkpaot-arm64.a KPAOTCC="$(A64_CC)" KPAOTAS="$(A64_AS)" KPAOTQEMU="$(A64_RUN)" KPAOTNOPIE="$(HOST_NO_PIE)"
KP_FREE_ENV_rv = MODE=free FRONTEND=build/skj-kpc-rv KPRV=build/skj-kpc-rv KPAS=build/skj-as-rv KPLD=build/skj-ld-rv KPCRT=build/kpfree-rv/kp_crt_rv.o KPLIB=build/libkpaot-rv-free.a QEMU="$(QEMU_RV)"

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
check-kp-aot: build/skj-kpc build/skj-kpc build/libkpaot.a
	@$(KP_AOT_ENV_x86_64) sh tests/run-kp-src-tests.sh

## Inline assembly in Kobold (asm('text')) is AOT only: the JIT does not
## assemble text.  This runs the asm fixture set through the native front end
## and the x86-64 backend AOT path, the same lane as check-kp-aot.
check-kp-asm: build/skj-kpc build/skj-kpc build/libkpaot.a
	@KP_SRCDIR=tests/kp-asm $(KP_AOT_ENV_x86_64) sh tests/run-kp-src-tests.sh

## The RV32 and AArch64 cross runs, under qemu.  JIT: the embedded front end JITs
## for the cross target in-process.  AOT: the host front end emits the module, the
## cross backend compiles and links it, qemu runs it.
check-kp-jit-rv: build/skj-kp-jit-rv
	@MODE=jit JIT=build/skj-kp-jit-rv QEMU="$(QEMU_RV)" sh tests/run-kp-src-tests.sh

check-kp-jit-arm64: build/skj-kp-jit-arm64
	@MODE=jit JIT=build/skj-kp-jit-arm64 QEMU="$(A64_RUN)" sh tests/run-kp-src-tests.sh

check-kp-aot-rv: build/skj-kpc-rv build/skj-kpc-rv build/libkpaot-rv.a
	@$(KP_AOT_ENV_rv) sh tests/run-kp-src-tests.sh

check-kp-aot-arm64: build/skj-kpc-arm64 build/skj-kpc-arm64 build/libkpaot-arm64.a $(A64_ASM_DEP)
	@$(KP_AOT_ENV_arm64) sh tests/run-kp-src-tests.sh

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
check-kp-corpus: build/skj-kp-jit build/skj-kpc build/skj-kpc build/libkpaot.a
	@QEMU= KP_SRCDIR=tests/kp-corpus MODE=jit JIT=build/skj-kp-jit sh tests/run-kp-src-tests.sh
	@KP_SRCDIR=tests/kp-corpus $(KP_AOT_ENV_x86_64) sh tests/run-kp-src-tests.sh

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
check-kp-corpus-rv: build/skj-kpc-rv build/skj-kpc-rv build/libkpaot-rv.a
	@KP_SRCDIR=tests/kp-corpus $(KP_AOT_ENV_rv) sh tests/run-kp-src-tests.sh

check-kp-corpus-arm64: build/skj-kpc-arm64 build/skj-kpc-arm64 build/libkpaot-arm64.a $(A64_ASM_DEP)
	@KP_SRCDIR=tests/kp-corpus $(KP_AOT_ENV_arm64) sh tests/run-kp-src-tests.sh

## The ColdFire corpus runs the hosted AOT path under qemu-m68k: skj-kpc-cf emits
## big-endian m68k asm from the agnostic seed, the glibc m68k gcc assembles and
## links it against libkpaot-cf.a, and qemu-m68k runs the program.  This is the
## big-endian cross-check of the whole corpus, the hosted sibling of the
## freestanding check-kp-free-cf (which runs the smaller kp-src set on skj-run).
## Linked -static: the glibc m68k driver would otherwise produce a dynamic ELF
## needing /lib/ld.so.1, which qemu-m68k has no sysroot for (every other
## qemu-m68k suite here is static/freestanding).  A static link needs no loader.
check-kp-corpus-cf: build/skj-kpc-cf build/skj-kpc-cf build/libkpaot-cf.a
	@KP_SRCDIR=tests/kp-corpus MODE=aot FRONTEND=build/skj-kpc-cf \
	    KPAOT=build/skj-kpc-cf KPAOTLIB=build/libkpaot-cf.a \
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
check-kp-multi: build/skj-kp-jit build/skj-kpc build/skj-kpc build/libkpaot.a
	@QEMU= KP_SRCDIR=tests/kp-multi MODE=jit JIT=build/skj-kp-jit sh tests/run-kp-multi-tests.sh
	@KP_SRCDIR=tests/kp-multi MODE=aot FRONTEND=build/skj-kpc \
	    KPAOT=build/skj-kpc KPAOTLIB=build/libkpaot.a \
	    KPAOTCC=$(CC) KPAOTAS="$(X64_ASM) $(X64_ASMFLAGS)" \
	    sh tests/run-kp-multi-tests.sh

check-kp-multi-rv: build/skj-kpc-rv build/skj-kpc-rv build/libkpaot-rv.a
	@KP_SRCDIR=tests/kp-multi MODE=aot FRONTEND=build/skj-kpc-rv \
	    KPAOT=build/skj-kpc-rv KPAOTLIB=build/libkpaot-rv.a \
	    KPAOTCC="$(RVJIT_CC)" KPAOTQEMU="$(QEMU_RV)" \
	    sh tests/run-kp-multi-tests.sh

check-kp-multi-arm64: build/skj-kpc-arm64 build/skj-kpc-arm64 build/libkpaot-arm64.a $(A64_ASM_DEP)
	@KP_SRCDIR=tests/kp-multi MODE=aot FRONTEND=build/skj-kpc-arm64 \
	    KPAOT=build/skj-kpc-arm64 KPAOTLIB=build/libkpaot-arm64.a \
	    KPAOTCC="$(A64_CC)" KPAOTAS="$(A64_AS)" \
	    KPAOTQEMU="$(A64_RUN)" KPAOTNOPIE="$(HOST_NO_PIE)" \
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

## io.textfile uses io.files, so it compiles with -U pointing at the directory
## whose io/ holds files.kpm.
build/kp-files/io/textfile.kpm: kobold/lib/io/textfile.pas build/kp-files/io/files.kpm build/skj-kpc | build
	@mkdir -p build/kp-files/io
	./build/skj-kpc -U build/kp-files --emit-kpm $< > $@

check-kp-files: build/skj-kp-jit build/skj-kpc build/skj-kpc-rv build/skj-kpc build/libkpaot.a \
                build/skj-kpc-rv build/skj-as-rv build/skj-ld-rv \
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
	@KOBOLD_UNITS=$(CURDIR)/build/kp-files KPAOT=build/skj-kpc KPAOTLIB=build/libkpaot.a \
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
	@./build/skj-kpc-rv build/kp-files/io/files-32.kpm -o build/kp-files/files_rv.s 2>>build/kp-files/rv.err
	@./build/skj-as-rv -o build/kp-files/files_rv.o build/kp-files/files_rv.s 2>>build/kp-files/rv.err
	@./build/skj-kpc-rv build/kp-files/filesrt-32.kpm -o build/kp-files/filesrt_rv.s 2>>build/kp-files/rv.err
	@./build/skj-as-rv -o build/kp-files/filesrt_rv.o build/kp-files/filesrt_rv.s 2>>build/kp-files/rv.err
	@./build/skj-ld-rv -o build/kp-files/filesrt_rv.elf build/kpfree-rv/kp_crt_rv.o \
	    build/kp-files/filesrt_rv.o build/kp-files/files_rv.o build/libkpaot-rv-free.a 2>>build/kp-files/rv.err
	@rm -f build/kp-files/rt.txt
	@$(QEMU_RV) build/kp-files/filesrt_rv.elf $(CURDIR)/build/kp-files/rt.txt \
	    > build/kp-files/free-rv.out 2>build/kp-files/free-rv.err; \
	 cmp -s build/kp-files/free-rv.out tests/kp-files/filesrt.expected \
	    || { echo "FAIL kp-files (free-rv)"; cat build/kp-files/rv.err build/kp-files/free-rv.err build/kp-files/free-rv.out; exit 1; }
	@echo "kp-files: 1 passed (jit, aot, free-rv)"

## The classic Pascal file verbs (io.textfile) over the same three tiers as
## check-kp-files: io.textfile is a thin unit over io.files, so both unit modules
## link into the program.  The freestanding runner does not walk `.uses`, so the
## rv32 tier compiles the program and both units width-4 and links them together.
check-kp-textfile: build/skj-kp-jit build/skj-kpc build/skj-kpc-rv build/skj-kpc build/libkpaot.a \
                   build/skj-kpc-rv build/skj-as-rv build/skj-ld-rv \
                   build/kpfree-rv/kp_crt_rv.o build/libkpaot-rv-free.a \
                   build/kp-files/io/files.kpm build/kp-files/io/textfile.kpm
	@cp tests/kp-files/textfilert.pas build/kp-files/textfilert.pas
	@rm -f build/kp-files/tf.txt
	@./build/skj-kp-jit build/kp-files/textfilert.pas $(CURDIR)/build/kp-files/tf.txt \
	    > build/kp-files/tf-jit.out 2>build/kp-files/tf-jit.err; \
	 cmp -s build/kp-files/tf-jit.out tests/kp-files/textfilert.expected \
	    || { echo "FAIL kp-textfile (jit)"; cat build/kp-files/tf-jit.err build/kp-files/tf-jit.out; exit 1; }
	@./build/skj-kpc -U build/kp-files --emit-kpm build/kp-files/textfilert.pas \
	    > build/kp-files/textfilert.kpm 2>build/kp-files/tf-emit.err
	@rm -f build/kp-files/tf.txt
	@KOBOLD_UNITS=$(CURDIR)/build/kp-files KPAOT=build/skj-kpc KPAOTLIB=build/libkpaot.a \
	    KPAOTCC=$(CC) KPAOTAS="$(X64_ASM) $(X64_ASMFLAGS)" \
	    sh kobold/aot-run.sh build/kp-files/textfilert.kpm $(CURDIR)/build/kp-files/tf.txt \
	    > build/kp-files/tf-aot.out 2>build/kp-files/tf-aot.err; \
	 cmp -s build/kp-files/tf-aot.out tests/kp-files/textfilert.expected \
	    || { echo "FAIL kp-textfile (aot)"; cat build/kp-files/tf-aot.err build/kp-files/tf-aot.out; exit 1; }
	@# freestanding rv32: re-emit the program and both units width-4 and link them.
	@./build/skj-kpc-rv --emit-kpm kobold/lib/io/files.pas > build/kp-files/io/files-32.kpm 2>build/kp-files/tf-rv.err
	@./build/skj-kpc-rv -U build/kp-files --emit-kpm kobold/lib/io/textfile.pas > build/kp-files/io/textfile-32.kpm 2>>build/kp-files/tf-rv.err
	@./build/skj-kpc-rv -U build/kp-files --emit-kpm build/kp-files/textfilert.pas > build/kp-files/textfilert-32.kpm 2>>build/kp-files/tf-rv.err
	@./build/skj-kpc-rv build/kp-files/io/files-32.kpm -o build/kp-files/tf_files_rv.s 2>>build/kp-files/tf-rv.err
	@./build/skj-as-rv -o build/kp-files/tf_files_rv.o build/kp-files/tf_files_rv.s 2>>build/kp-files/tf-rv.err
	@./build/skj-kpc-rv build/kp-files/io/textfile-32.kpm -o build/kp-files/tf_textfile_rv.s 2>>build/kp-files/tf-rv.err
	@./build/skj-as-rv -o build/kp-files/tf_textfile_rv.o build/kp-files/tf_textfile_rv.s 2>>build/kp-files/tf-rv.err
	@./build/skj-kpc-rv build/kp-files/textfilert-32.kpm -o build/kp-files/textfilert_rv.s 2>>build/kp-files/tf-rv.err
	@./build/skj-as-rv -o build/kp-files/textfilert_rv.o build/kp-files/textfilert_rv.s 2>>build/kp-files/tf-rv.err
	@./build/skj-ld-rv -o build/kp-files/textfilert_rv.elf build/kpfree-rv/kp_crt_rv.o \
	    build/kp-files/textfilert_rv.o build/kp-files/tf_textfile_rv.o build/kp-files/tf_files_rv.o \
	    build/libkpaot-rv-free.a 2>>build/kp-files/tf-rv.err
	@rm -f build/kp-files/tf.txt
	@$(QEMU_RV) build/kp-files/textfilert_rv.elf $(CURDIR)/build/kp-files/tf.txt \
	    > build/kp-files/tf-free-rv.out 2>build/kp-files/tf-free-rv.err; \
	 cmp -s build/kp-files/tf-free-rv.out tests/kp-files/textfilert.expected \
	    || { echo "FAIL kp-textfile (free-rv)"; cat build/kp-files/tf-rv.err build/kp-files/tf-free-rv.err build/kp-files/tf-free-rv.out; exit 1; }
	@echo "kp-textfile: 1 passed (jit, aot, free-rv)"

build/kp-dirs/io/dirs.kpm: kobold/lib/io/dirs.pas build/skj-kpc | build
	@mkdir -p build/kp-dirs/io
	./build/skj-kpc --emit-kpm $< > $@

## Directory reads reached through the `dirs` unit, over the same three tiers as
## check-kp-files.  A scratch directory of three known files is rebuilt before
## each runner; the program reports an order-independent entry count and total
## name length, so the expected output is deterministic.
check-kp-dirs: build/skj-kp-jit build/skj-kpc build/skj-kpc-rv build/skj-kpc build/libkpaot.a \
               build/skj-kpc-rv build/skj-as-rv build/skj-ld-rv \
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
	 KOBOLD_UNITS=$(CURDIR)/build/kp-dirs KPAOT=build/skj-kpc KPAOTLIB=build/libkpaot.a \
	     KPAOTCC=$(CC) KPAOTAS="$(X64_ASM) $(X64_ASMFLAGS)" \
	     sh kobold/aot-run.sh build/kp-dirs/dirsrt.kpm $$D \
	     > build/kp-dirs/aot.out 2>build/kp-dirs/aot.err; \
	 cmp -s build/kp-dirs/aot.out tests/kp-dirs/dirsrt.expected \
	     || { echo "FAIL kp-dirs (aot)"; cat build/kp-dirs/aot.err build/kp-dirs/aot.out; exit 1; }; \
	 ./build/skj-kpc-rv --emit-kpm kobold/lib/io/dirs.pas > build/kp-dirs/io/dirs-32.kpm 2>build/kp-dirs/rv.err; \
	 ./build/skj-kpc-rv --emit-kpm build/kp-dirs/dirsrt.pas > build/kp-dirs/dirsrt-32.kpm 2>>build/kp-dirs/rv.err; \
	 ./build/skj-kpc-rv build/kp-dirs/io/dirs-32.kpm -o build/kp-dirs/dirs_rv.s 2>>build/kp-dirs/rv.err; \
	 ./build/skj-as-rv -o build/kp-dirs/dirs_rv.o build/kp-dirs/dirs_rv.s 2>>build/kp-dirs/rv.err; \
	 ./build/skj-kpc-rv build/kp-dirs/dirsrt-32.kpm -o build/kp-dirs/dirsrt_rv.s 2>>build/kp-dirs/rv.err; \
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
## The rng/sched glue also ships in the hosted runtimes (libkpaot.a, the arm64
## sibling, and the in-process JIT, all -DKP_GLUE_HOSTED over the host libc
## clock/sleep/entropy), so std.time / std.random work on the hosted AOT and JIT
## tiers too; check-kp-stdlib exercises those.
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
build/kp-stdlib/std/threads.kpm: kobold/lib/std/threads.pas build/skj-kpc-rv | build
	@mkdir -p build/kp-stdlib/std
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
build/kp-stdlib-cf/std/threads.kpm: kobold/lib/std/threads.pas build/skj-kpc-cf | build
	@mkdir -p build/kp-stdlib-cf/std
	./build/skj-kpc-cf --emit-kpm $< > $@

## The hosted tiers for the same stddemo (std.random, std.time, io.console): the
## in-process x86-64 JIT and the native x86-64 AOT linked against libkpaot.a.
## This pins the std.time / std.random host glue (runtime/rng_glue.c,
## sched_glue.c) on the hosted paths; the glue formerly shipped only in the
## freestanding rv/cf archives, so a hosted `uses std.time`/`std.random` failed
## to link.  The .kpm modules are x86-64 (host skj-kpc) and are shared by both
## lanes (the JIT loads the same modules the AOT path links).
check-kp-stdlib: build/skj-kpc build/skj-kpc build/skj-kp-jit build/libkpaot.a build/skj-as-x86-64 \
                 build/skj-kpc-arm64 build/skj-kpc-arm64 build/libkpaot-arm64.a $(A64_ASM_DEP)
	@mkdir -p build/kp-stdlib-host/std build/kp-stdlib-host/io
	@cp tests/kp-stdlib/stddemo.pas build/kp-stdlib-host/stddemo.pas
	@E=build/kp-stdlib-host/build.err; : > $$E; \
	 for u in std/random std/time io/console; do \
	   ./build/skj-kpc --emit-kpm kobold/lib/$$u.pas > build/kp-stdlib-host/$$u.kpm 2>>$$E || \
	     { echo "FAIL kp-stdlib (emit $$u)"; cat $$E; exit 1; }; \
	 done; \
	 ./build/skj-kp-jit build/kp-stdlib-host/stddemo.pas </dev/null \
	   > build/kp-stdlib-host/jit.out 2>>$$E; \
	 cmp -s build/kp-stdlib-host/jit.out tests/kp-stdlib/stddemo.expected \
	   || { echo "FAIL kp-stdlib (jit)"; cat $$E build/kp-stdlib-host/jit.out; exit 1; }; \
	 ./build/skj-kpc -U build/kp-stdlib-host --emit-kpm build/kp-stdlib-host/stddemo.pas \
	   > build/kp-stdlib-host/stddemo.kpm 2>>$$E || \
	     { echo "FAIL kp-stdlib (emit stddemo)"; cat $$E; exit 1; }; \
	 KOBOLD_UNITS=$(CURDIR)/build/kp-stdlib-host KPAOT=build/skj-kpc KPAOTLIB=build/libkpaot.a \
	   KPAOTCC=$(CC) KPAOTAS="$(X64_ASM) $(X64_ASMFLAGS)" \
	   sh kobold/aot-run.sh build/kp-stdlib-host/stddemo.kpm </dev/null \
	   > build/kp-stdlib-host/aot.out 2>>$$E; \
	 cmp -s build/kp-stdlib-host/aot.out tests/kp-stdlib/stddemo.expected \
	   || { echo "FAIL kp-stdlib (aot)"; cat $$E build/kp-stdlib-host/aot.out; exit 1; }
	@# arm64 native AOT (the mac's NATIVE_LIBKPAOT): the same glue must ship in
	@# libkpaot-arm64.a; run under qemu-aarch64.  Units are target-bound, so emit
	@# arm64 .kpm separately with the arm64 front end.
	@mkdir -p build/kp-stdlib-host/arm64/std build/kp-stdlib-host/arm64/io
	@E=build/kp-stdlib-host/build.err; \
	 for u in std/random std/time io/console; do \
	   ./build/skj-kpc-arm64 --emit-kpm kobold/lib/$$u.pas > build/kp-stdlib-host/arm64/$$u.kpm 2>>$$E || \
	     { echo "FAIL kp-stdlib (emit arm64 $$u)"; cat $$E; exit 1; }; \
	 done; \
	 ./build/skj-kpc-arm64 -U build/kp-stdlib-host/arm64 --emit-kpm build/kp-stdlib-host/stddemo.pas \
	   > build/kp-stdlib-host/arm64/stddemo.kpm 2>>$$E || \
	     { echo "FAIL kp-stdlib (emit arm64 stddemo)"; cat $$E; exit 1; }; \
	 KOBOLD_UNITS=$(CURDIR)/build/kp-stdlib-host/arm64 KPAOT=build/skj-kpc-arm64 \
	   KPAOTLIB=build/libkpaot-arm64.a KPAOTCC="$(A64_CC)" KPAOTAS="$(A64_AS)" \
	   KPAOTQEMU="$(A64_RUN)" KPAOTNOPIE="$(HOST_NO_PIE)" \
	   sh kobold/aot-run.sh build/kp-stdlib-host/arm64/stddemo.kpm </dev/null \
	   > build/kp-stdlib-host/arm64.out 2>>$$E; \
	 cmp -s build/kp-stdlib-host/arm64.out tests/kp-stdlib/stddemo.expected \
	   || { echo "FAIL kp-stdlib (aot-arm64)"; cat $$E build/kp-stdlib-host/arm64.out; exit 1; }
	@echo "kp-stdlib: 3 passed (jit, aot-x86-64, aot-arm64): std.random, std.time, io.console"

check-kp-stdlib-free: build/skj-kpc-rv build/skj-kpc-rv build/skj-as-rv build/skj-ld-rv \
                      build/kpfree-rv/kp_crt_rv.o build/libkpaot-rv-free.a \
                      build/kp-stdlib/io/console.kpm build/kp-stdlib/io/buffered.kpm \
                      build/kp-stdlib/std/time.kpm \
                      build/kp-stdlib/std/random.kpm build/kp-stdlib/core/strings.kpm \
                      build/kp-stdlib/core/convert.kpm build/kp-stdlib/std/threads.kpm
	@cp tests/kp-stdlib/stddemo.pas build/kp-stdlib/stddemo.pas
	@E=build/kp-stdlib/build.err; : > $$E; \
	 ./build/skj-kpc-rv --emit-kpm build/kp-stdlib/stddemo.pas > build/kp-stdlib/stddemo.kpm 2>>$$E; \
	 for u in io/console std/time std/random stddemo; do \
	   b=$$(basename $$u); \
	   ./build/skj-kpc-rv build/kp-stdlib/$$u.kpm -o build/kp-stdlib/$$b.s 2>>$$E && \
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
	 ./build/skj-kpc-rv build/kp-stdlib/consoleio.kpm -o build/kp-stdlib/consoleio.s 2>>$$E && \
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
	 ./build/skj-kpc-rv build/kp-stdlib/io/buffered.kpm -o build/kp-stdlib/buffered.s 2>>$$E && \
	 ./build/skj-as-rv -o build/kp-stdlib/buffered.o build/kp-stdlib/buffered.s 2>>$$E || \
	   { echo "FAIL kp-stdlib (compile io.buffered)"; cat $$E; exit 1; }; \
	 ./build/skj-kpc-rv build/kp-stdlib/bufferedio.kpm -o build/kp-stdlib/bufferedio.s 2>>$$E && \
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
	 ./build/skj-kpc-rv build/kp-stdlib/realunit.kpm -o build/kp-stdlib/realunit.s 2>>$$E && \
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
	 ./build/skj-kpc-rv build/kp-stdlib/nswild.kpm -o build/kp-stdlib/nswild.s 2>>$$E && \
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
	 ./build/skj-kpc-rv build/kp-stdlib/strdemo.kpm -o build/kp-stdlib/strdemo.s 2>>$$E && \
	 ./build/skj-as-rv -o build/kp-stdlib/strdemo.o build/kp-stdlib/strdemo.s 2>>$$E || \
	   { echo "FAIL kp-stdlib (assemble strdemo)"; cat $$E; exit 1; }; \
	 ./build/skj-kpc-rv build/kp-stdlib/core/strings.kpm -o build/kp-stdlib/strings.s 2>>$$E && \
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
	 ./build/skj-kpc-rv build/kp-stdlib/convertnum.kpm -o build/kp-stdlib/convertnum.s 2>>$$E && \
	 ./build/skj-as-rv -o build/kp-stdlib/convertnum.o build/kp-stdlib/convertnum.s 2>>$$E || \
	   { echo "FAIL kp-stdlib (assemble convertnum)"; cat $$E; exit 1; }; \
	 ./build/skj-kpc-rv build/kp-stdlib/core/convert.kpm -o build/kp-stdlib/convert.s 2>>$$E && \
	 ./build/skj-as-rv -o build/kp-stdlib/convert.o build/kp-stdlib/convert.s 2>>$$E || \
	   { echo "FAIL kp-stdlib (assemble core.convert)"; cat $$E; exit 1; }; \
	 ./build/skj-ld-rv -o build/kp-stdlib/convertnum.elf build/kpfree-rv/kp_crt_rv.o \
	   build/kp-stdlib/convertnum.o build/kp-stdlib/convert.o build/kp-stdlib/console.o \
	   build/libkpaot-rv-free.a 2>>$$E || \
	     { echo "FAIL kp-stdlib (link convertnum)"; cat $$E; exit 1; }; \
	 $(QEMU_RV) build/kp-stdlib/convertnum.elf </dev/null > build/kp-stdlib/convout 2>>$$E; \
	 cmp -s build/kp-stdlib/convout tests/kp-stdlib/convertnum.expected \
	   || { echo "FAIL kp-stdlib (convertnum)"; cat $$E build/kp-stdlib/convout; exit 1; }
	@cp tests/kp-stdlib/threaddemo.pas build/kp-stdlib/threaddemo.pas
	@E=build/kp-stdlib/build.err; : > $$E; \
	 ./build/skj-kpc-rv --emit-kpm build/kp-stdlib/threaddemo.pas > build/kp-stdlib/threaddemo.kpm 2>>$$E || \
	   { echo "FAIL kp-stdlib (compile threaddemo: std.threads)"; cat $$E; exit 1; }; \
	 ./build/skj-kpc-rv build/kp-stdlib/threaddemo.kpm -o build/kp-stdlib/threaddemo.s 2>>$$E && \
	 ./build/skj-as-rv -o build/kp-stdlib/threaddemo.o build/kp-stdlib/threaddemo.s 2>>$$E || \
	   { echo "FAIL kp-stdlib (assemble threaddemo)"; cat $$E; exit 1; }; \
	 ./build/skj-kpc-rv build/kp-stdlib/std/threads.kpm -o build/kp-stdlib/threads.s 2>>$$E && \
	 ./build/skj-as-rv -o build/kp-stdlib/threads.o build/kp-stdlib/threads.s 2>>$$E || \
	   { echo "FAIL kp-stdlib (assemble std.threads)"; cat $$E; exit 1; }; \
	 ./build/skj-ld-rv -o build/kp-stdlib/threaddemo.elf build/kpfree-rv/kp_crt_rv.o \
	   build/kp-stdlib/threaddemo.o build/kp-stdlib/threads.o build/libkpaot-rv-free.a 2>>$$E || \
	     { echo "FAIL kp-stdlib (link threaddemo)"; cat $$E; exit 1; }; \
	 $(QEMU_RV) build/kp-stdlib/threaddemo.elf </dev/null > build/kp-stdlib/threadout 2>>$$E; \
	 cmp -s build/kp-stdlib/threadout tests/kp-stdlib/threaddemo.expected \
	   || { echo "FAIL kp-stdlib (threaddemo)"; cat $$E build/kp-stdlib/threadout; exit 1; }
	@echo "kp-stdlib: 8 passed (free-rv): std.random (+ real/secure), std.time, io.console (+ read/write, real), io.buffered, realunit, uses ns.*, core.strings, core.convert, std.threads"

## The ColdFire sibling of check-kp-stdlib-free: the same stddemo (std.random,
## std.time, io.console) over the cf freestanding tier under skj-run, proving the
## ColdFire time/rng/yield syscall wrappers (syscall_cf.S) and the emulator's
## GSYS_CLOCK_GETTIME / GSYS_GETRANDOM / GSYS_NANOSLEEP maps (emu/cf_user.c).  The
## .kpm modules are arch-neutral (host skj-kpc), so only the backend and the link
## differ from the rv target.  The unit .kpm modules are cf-specific (build/
## kp-stdlib-cf, emitted by the ColdFire front end), since a unit is bound to its
## target; outputs go there too so the rv objects are not clobbered.
check-kp-stdlib-free-cf: build/skj-kpc-cf build/skj-kpc-cf build/skj-as build/skj-ld \
                         build/skj-run build/kpfree-cf/kp_crt_cf.o build/libkpaot-cf-free.a \
                         build/kp-stdlib-cf/io/console.kpm build/kp-stdlib-cf/std/time.kpm \
                         build/kp-stdlib-cf/std/random.kpm build/kp-stdlib-cf/core/convert.kpm \
                         build/kp-stdlib-cf/std/threads.kpm
	@mkdir -p build/kp-stdlib-cf
	@cp tests/kp-stdlib/stddemo.pas build/kp-stdlib-cf/stddemo.pas
	@E=build/kp-stdlib-cf/build.err; : > $$E; \
	 ./build/skj-kpc-cf -U build/kp-stdlib-cf --emit-kpm build/kp-stdlib-cf/stddemo.pas > build/kp-stdlib-cf/stddemo.kpm 2>>$$E; \
	 for u in io/console std/time std/random; do \
	   b=$$(basename $$u); \
	   ./build/skj-kpc-cf build/kp-stdlib-cf/$$u.kpm -o build/kp-stdlib-cf/$$b.s 2>>$$E && \
	   ./build/skj-as -o build/kp-stdlib-cf/$$b.o build/kp-stdlib-cf/$$b.s 2>>$$E || \
	     { echo "FAIL kp-stdlib-cf (compile $$u)"; cat $$E; exit 1; }; \
	 done; \
	 ./build/skj-kpc-cf build/kp-stdlib-cf/stddemo.kpm -o build/kp-stdlib-cf/stddemo.s 2>>$$E && \
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
	 ./build/skj-kpc-cf build/kp-stdlib-cf/core/convert.kpm -o build/kp-stdlib-cf/convert.s 2>>$$E && \
	 ./build/skj-as -o build/kp-stdlib-cf/convert.o build/kp-stdlib-cf/convert.s 2>>$$E || \
	   { echo "FAIL kp-stdlib-cf (assemble core.convert)"; cat $$E; exit 1; }; \
	 ./build/skj-kpc-cf build/kp-stdlib-cf/convertnum.kpm -o build/kp-stdlib-cf/convertnum.s 2>>$$E && \
	 ./build/skj-as -o build/kp-stdlib-cf/convertnum.o build/kp-stdlib-cf/convertnum.s 2>>$$E || \
	   { echo "FAIL kp-stdlib-cf (assemble convertnum)"; cat $$E; exit 1; }; \
	 ./build/skj-ld -T kobold/kp_cf.ld -o build/kp-stdlib-cf/convertnum.elf build/kpfree-cf/kp_crt_cf.o \
	   build/kp-stdlib-cf/convertnum.o build/kp-stdlib-cf/convert.o build/kp-stdlib-cf/console.o \
	   build/libkpaot-cf-free.a 2>>$$E || \
	     { echo "FAIL kp-stdlib-cf (link convertnum)"; cat $$E; exit 1; }; \
	 build/skj-run build/kp-stdlib-cf/convertnum.elf </dev/null > build/kp-stdlib-cf/convout 2>>$$E; \
	 cmp -s build/kp-stdlib-cf/convout tests/kp-stdlib/convertnum.expected \
	   || { echo "FAIL kp-stdlib-cf (convertnum)"; cat $$E build/kp-stdlib-cf/convout; exit 1; }
	@cp tests/kp-stdlib/threaddemo.pas build/kp-stdlib-cf/threaddemo.pas
	@E=build/kp-stdlib-cf/build.err; : > $$E; \
	 ./build/skj-kpc-cf -U build/kp-stdlib-cf --emit-kpm build/kp-stdlib-cf/threaddemo.pas > build/kp-stdlib-cf/threaddemo.kpm 2>>$$E || \
	   { echo "FAIL kp-stdlib-cf (compile threaddemo: std.threads)"; cat $$E; exit 1; }; \
	 ./build/skj-kpc-cf build/kp-stdlib-cf/threaddemo.kpm -o build/kp-stdlib-cf/threaddemo.s 2>>$$E && \
	 ./build/skj-as -o build/kp-stdlib-cf/threaddemo.o build/kp-stdlib-cf/threaddemo.s 2>>$$E || \
	   { echo "FAIL kp-stdlib-cf (assemble threaddemo)"; cat $$E; exit 1; }; \
	 ./build/skj-kpc-cf build/kp-stdlib-cf/std/threads.kpm -o build/kp-stdlib-cf/threads.s 2>>$$E && \
	 ./build/skj-as -o build/kp-stdlib-cf/threads.o build/kp-stdlib-cf/threads.s 2>>$$E || \
	   { echo "FAIL kp-stdlib-cf (assemble std.threads)"; cat $$E; exit 1; }; \
	 ./build/skj-ld -T kobold/kp_cf.ld -o build/kp-stdlib-cf/threaddemo.elf build/kpfree-cf/kp_crt_cf.o \
	   build/kp-stdlib-cf/threaddemo.o build/kp-stdlib-cf/threads.o build/libkpaot-cf-free.a 2>>$$E || \
	     { echo "FAIL kp-stdlib-cf (link threaddemo)"; cat $$E; exit 1; }; \
	 build/skj-run build/kp-stdlib-cf/threaddemo.elf </dev/null > build/kp-stdlib-cf/threadout 2>>$$E; \
	 cmp -s build/kp-stdlib-cf/threadout tests/kp-stdlib/threaddemo.expected \
	   || { echo "FAIL kp-stdlib-cf (threaddemo)"; cat $$E build/kp-stdlib-cf/threadout; exit 1; }
	@echo "kp-stdlib-cf: 3 passed (free-cf): std.random/std.time/io.console over the ColdFire time/rng seam, core.convert, std.threads"

## ---- the input-dependent corpus (tests/kp-io) ----
## Programs that read stdin or take arguments: a .input file is the program's
## stdin and a .args file its command line (ParamStr/ParamCount), with the
## .expected the reference kprun's output under the same.  This is the one surface
## the other corpora omit (they run with no input), so it exercises the runner's
## stdin/argv plumbing across every tier.  x86 JIT and AOT are in check-all; the
## rv32 and arm64 AOT paths and the freestanding tier run in check-emu-all, so the
## entry argument vector and the read seam are checked on the no-libc tier too.
check-kp-io: build/skj-kp-jit build/skj-kpc build/skj-kpc build/libkpaot.a
	@QEMU= KP_SRCDIR=tests/kp-io MODE=jit JIT=build/skj-kp-jit sh tests/run-kp-src-tests.sh
	@KP_SRCDIR=tests/kp-io $(KP_AOT_ENV_x86_64) sh tests/run-kp-src-tests.sh

check-kp-io-rv: build/skj-kpc-rv build/skj-kpc-rv build/libkpaot-rv.a
	@KP_SRCDIR=tests/kp-io $(KP_AOT_ENV_rv) sh tests/run-kp-src-tests.sh

check-kp-io-arm64: build/skj-kpc-arm64 build/skj-kpc-arm64 build/libkpaot-arm64.a $(A64_ASM_DEP)
	@KP_SRCDIR=tests/kp-io $(KP_AOT_ENV_arm64) sh tests/run-kp-src-tests.sh

check-kp-io-free: build/skj-kpc-rv build/skj-kpc-rv build/skj-as-rv build/skj-ld-rv \
                  build/kpfree-rv/kp_crt_rv.o build/libkpaot-rv-free.a
	@KP_SRCDIR=tests/kp-io $(KP_FREE_ENV_rv) sh tests/run-kp-src-tests.sh

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
check-kp-inc: build/skj-kp-jit build/skj-kpc build/skj-kpc build/libkpaot.a
	@QEMU= KP_SRCDIR=tests/kp-inc MODE=jit JIT=build/skj-kp-jit sh tests/run-kp-src-tests.sh
	@KP_SRCDIR=tests/kp-inc $(KP_AOT_ENV_x86_64) sh tests/run-kp-src-tests.sh

check-kp-inc-rv: build/skj-kpc-rv build/skj-kpc-rv build/libkpaot-rv.a
	@KP_SRCDIR=tests/kp-inc $(KP_AOT_ENV_rv) sh tests/run-kp-src-tests.sh

check-kp-inc-arm64: build/skj-kpc-arm64 build/skj-kpc-arm64 build/libkpaot-arm64.a $(A64_ASM_DEP)
	@KP_SRCDIR=tests/kp-inc $(KP_AOT_ENV_arm64) sh tests/run-kp-src-tests.sh

check-kp-inc-free: build/skj-kpc-rv build/skj-kpc-rv build/skj-as-rv build/skj-ld-rv \
                   build/kpfree-rv/kp_crt_rv.o build/libkpaot-rv-free.a
	@KP_SRCDIR=tests/kp-inc $(KP_FREE_ENV_rv) sh tests/run-kp-src-tests.sh

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
## -mno-relax so skj-ld-rv links it, double reals to match skj-kpc-rv, and
## KP_FREESTANDING for the glue.
# Deferred (=, not :=) because RV_RT_CFLAGS is defined further down the Makefile.
# The C runtime is compiled by skj-cc-rv to GAS asm and assembled by skj-as-rv,
# so the tier needs no external toolchain (smolmoo/aldeby self-build it from the
# skj toolchain alone), matching the ColdFire tier.  skj-cc-rv ignores the
# gcc-style flags, so only the defines and the include path carry over; it emits
# the RISC-V ILP32 psABI, which the hand-written .S pieces below also use.
KPFREE_RV_SKJFLAGS := -DKP_FREESTANDING -DKP_REAL=double -Ikobold -Iir

build/kpfree-rv/%.o: kobold/%.c $(KP_HDRS_RV) build/skj-cc-rv build/skj-as-rv Makefile | build/kpfree-rv
	./build/skj-cc-rv $(KPFREE_RV_SKJFLAGS) -o $(@:.o=.s) $<
	./build/skj-as-rv -o $@ $(@:.o=.s)

# The single-step module builder the freestanding self-host compiler links: the
# in-memory IR builder (ir/), the C module builder (kpbuild.c + kpb_fe.c), the
# builder's output driver (kpb_native.c, KPM only here, no backend), and the KPM
# serializer and reader (kpm_write.c, kpm.c).  These let the in-guest kpcom lower
# straight into the builder and --emit-kpm through kpm_write, with no seed.  The
# kobold/ sources above already match the kobold pattern rule; ir/ needs its own.
build/kpfree-rv/%.o: ir/%.c $(wildcard ir/*.h) build/skj-cc-rv build/skj-as-rv Makefile | build/kpfree-rv
	./build/skj-cc-rv $(KPFREE_RV_SKJFLAGS) -o $(@:.o=.s) $<
	./build/skj-as-rv -o $@ $(@:.o=.s)

# soft64.c supplies only the 64-bit divide family here (-DSOFT64_NO_MULSHIFT);
# the mul/shift helpers are the hand-written .S aliases below, which the rv
# backend's internal stack ABI calls.
build/kpfree-rv/soft64.o: runtime/soft64.c build/skj-cc-rv build/skj-as-rv Makefile | build/kpfree-rv
	./build/skj-cc-rv $(KPFREE_RV_SKJFLAGS) -DSOFT64_NO_MULSHIFT -o $(@:.o=.s) runtime/soft64.c
	./build/skj-as-rv -o $@ $(@:.o=.s)

# The other runtime/ sources the archive pulls in with the plain freestanding
# flags: the RNG generators and their guest-facing glue (rng, rng_glue) and the
# park / yield glue (sched_glue, Sleep / Yield over nanosleep / sched_yield).
# One pattern rule covers them; soft64.o keeps its explicit rule above for the
# -DSOFT64_NO_MULSHIFT it needs, and an explicit rule wins over this pattern.
build/kpfree-rv/%.o: runtime/%.c $(wildcard runtime/*.h) build/skj-cc-rv build/skj-as-rv Makefile | build/kpfree-rv
	./build/skj-cc-rv $(KPFREE_RV_SKJFLAGS) -o $(@:.o=.s) $<
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

# The plain libc/ sources the freestanding archive pulls in with the libc
# include path only (no FS flags): stdio_core (the buffered-stream core the
# Kobold and C runtimes share), strtod and ctype (io_free.c's scan_real path),
# numfmt (the libgcc-free formatters), and sbrk below.  One pattern rule covers
# them; stat.c and fileops.c come from libc/ too but keep explicit rules with
# the FS flags, which take precedence over this pattern.
build/kpfree-rv/%.o: libc/%.c $(wildcard include/*.h) $(wildcard libc/*.h) build/skj-cc-rv build/skj-as-rv Makefile | build/kpfree-rv
	./build/skj-cc-rv -Iinclude -Ilibc -o $(@:.o=.s) $<
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
## kp_rt_meta.c's path-metadata ops (the kpf_exists family) need struct stat and
## stat/mkdir, so like io_free.o it is built with the libc include path on top of
## the freestanding flags.  It takes no FS flags: it calls stat/unlink/rename/
## mkdir/rmdir directly (the stat.o/fileops.o wrappers above supply them over the
## crt's statx and *at syscalls), so this is the one hosted-and-freestanding
## shared source in the archive.
build/kpfree-rv/kp_rt_meta.o: kobold/kp_rt_meta.c $(KP_HDRS_RV) $(wildcard include/*.h) $(wildcard include/sys/*.h) build/skj-cc-rv build/skj-as-rv Makefile | build/kpfree-rv
	./build/skj-cc-rv $(KPFREE_RV_SKJFLAGS) -Iinclude -Ilibc -o $(@:.o=.s) kobold/kp_rt_meta.c
	./build/skj-as-rv -o $@ $(@:.o=.s)
## The brk-bump both runtimes share: rt_free.c's gheap grows through __libc_sbrk
## (libc/sbrk.c), the same source libc's malloc uses.  skj-cc-rv defines
## __skj_ilp32__, so sbrk.c takes its brk path, not the LP64 fixed-region one.
## Built by the libc/%.o pattern rule above.

KPFREE_RV_ARCHIVE := build/kpfree-rv/rt_free.o build/kpfree-rv/io_free.o build/kpfree-rv/kp_rt_meta.o build/kpfree-rv/kp_io.o \
                     build/kpfree-rv/io_free_rv.o build/kpfree-rv/sbrk.o \
                     build/kpfree-rv/fs.o build/kpfree-rv/ramfs.o build/kpfree-rv/ambientfs.o build/kpfree-rv/hostcap.o \
                     build/kpfree-rv/devfs.o build/kpfree-rv/romfs.o \
                     build/kpfree-rv/ninefs.o build/kpfree-rv/p9_guest.o \
                     build/kpfree-rv/numfmt.o build/kpfree-rv/stat.o build/kpfree-rv/fileops.o \
                     build/kpfree-rv/strtod.o build/kpfree-rv/ctype.o \
                     build/kpfree-rv/str_core.o build/kpfree-rv/dynarr.o \
                     build/kpfree-rv/map.o build/kpfree-rv/soft64.o \
                     build/kpfree-rv/rv_i64_aot_alias.o build/kpfree-rv/rv_jit_i64.o \
                     build/kpfree-rv/co_free.o build/kpfree-rv/co_switch_rv.o \
                     build/kpfree-rv/math_free.o build/kpfree-rv/rng.o \
                     build/kpfree-rv/rng_glue.o build/kpfree-rv/sched_glue.o \
                     build/kpfree-rv/kpbuild.o build/kpfree-rv/kpb_fe.o \
                     build/kpfree-rv/kpb_native.o build/kpfree-rv/kpm_write.o \
                     build/kpfree-rv/kpm.o build/kpfree-rv/ir.o \
                     build/kpfree-rv/util.o build/kpfree-rv/arena.o \
                     build/kpfree-rv/stdio_core.o build/kpfree-rv/stdio.o \
                     build/kpfree-rv/printf.o build/kpfree-rv/va_rv.o

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
KP_HDRS_CF := kobold/str.h kobold/dynarr.h kobold/map.h kobold/obj.h kobold/gheap_core.h kobold/co.h kobold/kp_rt_io.h \
              kobold/handle.h kobold/lowmem.h kobold/rt_glue.h kobold/kp_abi.h ir/target.h
# The C runtime is compiled by skj-cc (the in-tree C compiler) to GAS asm and
# assembled by skj-as, so the ColdFire kp runtime is built with no external
# toolchain (the host-seam syscalls and the fp0 math shim are already skj-as .S).
# skj-cc ignores the gcc-style flags, so only the defines and the include path
# carry over; strict-ColdFire is implicit (the backend targets ColdFire/m68k).
KPFREE_CF_SKJFLAGS := -DKP_FREESTANDING -DKP_REAL=double -Ikobold -Iir

build/kpfree-cf/%.o: kobold/%.c $(KP_HDRS_CF) build/skj-cc build/skj-as Makefile | build/kpfree-cf
	./build/skj-cc $(KPFREE_CF_SKJFLAGS) -o $(@:.o=.s) $<
	./build/skj-as -o $@ $(@:.o=.s)

# The single-step module builder the freestanding self-host compiler links (the
# cf sibling of the rv ir/%.o rule): the in-memory IR builder (ir/) feeding the C
# module builder and KPM serializer in the kobold/ pattern above.
build/kpfree-cf/%.o: ir/%.c $(wildcard ir/*.h) build/skj-cc build/skj-as Makefile | build/kpfree-cf
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

# The plain libc/ sources for the cf archive (the sibling of the rv libc/%.o
# pattern rule): stdio_core, strtod, ctype, numfmt, and sbrk below, all with the
# libc include path only.  stat.c and fileops.c keep explicit FS-flag rules,
# which take precedence over this pattern.
build/kpfree-cf/%.o: libc/%.c $(wildcard include/*.h) $(wildcard libc/*.h) build/skj-cc build/skj-as Makefile | build/kpfree-cf
	./build/skj-cc -Iinclude -Ilibc -o $(@:.o=.s) $<
	./build/skj-as -o $@ $(@:.o=.s)

## io_free.c's file-metadata ops need struct stat and stat/mkdir (see the rv rule).
## Kobold freestanding file I/O through the shared fs/ layer (the cf sibling of
## the rv rules): the Kobold-side io_free.c shim plus the shared stat.c/fileops.c
## path ops, built with SKJ_FS_INTERPOSE and the temporary ambient host backend.
KPFREE_CF_FS := -DSKJ_FS_INTERPOSE -DFS_WITH_AMBIENT -DFS_WITH_HOSTCAP -Ifs
FS_FREE_CC_CF := ./build/skj-cc -Iinclude -Ilibc -Ifs -DFS_WITH_AMBIENT -DFS_WITH_HOSTCAP -DFS_AMBIENT_DEFAULT -DFS_WITH_DEV -DFS_WITH_ROM -DFS_WITH_9P -DFS_WITH_STREAMS -DFS_WITH_RAM
build/kpfree-cf/io_free.o: kobold/io_free.c $(KP_HDRS_CF) $(FS_HDR) $(wildcard include/*.h) $(wildcard include/sys/*.h) build/skj-cc build/skj-as Makefile | build/kpfree-cf
	./build/skj-cc $(KPFREE_CF_SKJFLAGS) -Iinclude -Ilibc $(KPFREE_CF_FS) -o $(@:.o=.s) kobold/io_free.c
	./build/skj-as -o $@ $(@:.o=.s)
build/kpfree-cf/stat.o: libc/stat.c $(FS_HDR) $(wildcard include/*.h) $(wildcard include/sys/*.h) build/skj-cc build/skj-as Makefile | build/kpfree-cf
	./build/skj-cc -Iinclude -Ilibc $(KPFREE_CF_FS) -o $(@:.o=.s) libc/stat.c
	./build/skj-as -o $@ $(@:.o=.s)
build/kpfree-cf/fileops.o: libc/fileops.c $(FS_HDR) $(wildcard include/*.h) $(wildcard include/sys/*.h) build/skj-cc build/skj-as Makefile | build/kpfree-cf
	./build/skj-cc -Iinclude -Ilibc $(KPFREE_CF_FS) -o $(@:.o=.s) libc/fileops.c
	./build/skj-as -o $@ $(@:.o=.s)
## kp_rt_meta.c's path-metadata ops need struct stat and stat/mkdir (see the rv
## rule): the libc include path on top of the freestanding flags, no FS flags.
build/kpfree-cf/kp_rt_meta.o: kobold/kp_rt_meta.c $(KP_HDRS_CF) $(wildcard include/*.h) $(wildcard include/sys/*.h) build/skj-cc build/skj-as Makefile | build/kpfree-cf
	./build/skj-cc $(KPFREE_CF_SKJFLAGS) -Iinclude -Ilibc -o $(@:.o=.s) kobold/kp_rt_meta.c
	./build/skj-as -o $@ $(@:.o=.s)
# The fs/ layer objects for the cf freestanding archive (the sibling of the rv
# fs/%.o pattern rule): one source per target stem, full FS feature set, skj-as.
build/kpfree-cf/%.o: fs/%.c $(FS_HDR) $(wildcard include/*.h) build/skj-cc build/skj-as Makefile | build/kpfree-cf
	$(FS_FREE_CC_CF) -o $(@:.o=.s) $<
	./build/skj-as -o $@ $(@:.o=.s)
## The shared brk-bump (see the rv rule above): rt_free.c's gheap over
## __libc_sbrk.  Built by the libc/%.o pattern rule above.

# The other runtime/ sources for the cf archive (the sibling of the rv
# runtime/%.o pattern rule): the RNG generators and glue (rng, rng_glue) and the
# park / yield glue (sched_glue), over the ColdFire time/rng/yield syscall
# wrappers in syscall_cf.S.  soft64.o keeps its explicit rule above and wins
# over this pattern.
build/kpfree-cf/%.o: runtime/%.c $(wildcard runtime/*.h) build/skj-cc build/skj-as Makefile | build/kpfree-cf
	./build/skj-cc $(KPFREE_CF_SKJFLAGS) -o $(@:.o=.s) $<
	./build/skj-as -o $@ $(@:.o=.s)

KPFREE_CF_ARCHIVE := build/kpfree-cf/rt_free.o build/kpfree-cf/io_free.o build/kpfree-cf/kp_rt_meta.o build/kpfree-cf/kp_io.o \
                     build/kpfree-cf/io_free_cf.o build/kpfree-cf/sbrk.o \
                     build/kpfree-cf/fs.o build/kpfree-cf/ramfs.o build/kpfree-cf/ambientfs.o build/kpfree-cf/hostcap.o \
                     build/kpfree-cf/devfs.o build/kpfree-cf/romfs.o \
                     build/kpfree-cf/ninefs.o build/kpfree-cf/p9_guest.o \
                     build/kpfree-cf/numfmt.o build/kpfree-cf/stat.o build/kpfree-cf/fileops.o \
                     build/kpfree-cf/strtod.o build/kpfree-cf/ctype.o \
                     build/kpfree-cf/str_core.o build/kpfree-cf/dynarr.o \
                     build/kpfree-cf/map.o build/kpfree-cf/soft64.o \
                     build/kpfree-cf/co_free.o build/kpfree-cf/math_free.o \
                     build/kpfree-cf/rng.o build/kpfree-cf/rng_glue.o \
                     build/kpfree-cf/sched_glue.o \
                     build/kpfree-cf/kpbuild.o build/kpfree-cf/kpb_fe.o \
                     build/kpfree-cf/kpb_native.o build/kpfree-cf/kpm_write.o \
                     build/kpfree-cf/kpm.o build/kpfree-cf/ir.o \
                     build/kpfree-cf/util.o build/kpfree-cf/arena.o \
                     build/kpfree-cf/stdio_core.o build/kpfree-cf/stdio.o \
                     build/kpfree-cf/printf.o build/kpfree-cf/va_cf.o \
                     build/co_switch_cf.o

build/libkpaot-cf-free.a: $(KPFREE_CF_ARCHIVE) | build/kpfree-cf
	@rm -f $@
	$(AR) rcs $@ $(KPFREE_CF_ARCHIVE)

## check-kp-free: the boris definition-of-done pipeline, from Kobold source to a
## freestanding rv32 ELF, with nothing but the skj toolchain past the front end.
## The single-step skj-kpc-rv compiles the source to rv32 asm, skj-as-rv assembles,
## and skj-ld-rv links the crt, the program, and libkpaot-rv-free.a into a static
## ELF whose only host needs are write/read/exit and the handed-in heap.  Run under
## qemu-riscv32 (which trivially provides that seam); stdout must match .expected.
check-kp-free: build/skj-kpc-rv build/skj-kpc-rv build/skj-as-rv build/skj-ld-rv \
               build/kpfree-rv/kp_crt_rv.o build/libkpaot-rv-free.a
	@$(KP_FREE_ENV_rv) sh tests/run-kp-src-tests.sh

## The ColdFire freestanding kp AOT path, run on the in-tree emulator (qemu does
## not model CFV4e).  skj-kpc-cf emits big-endian ColdFire asm from the one
## agnostic seed, skj-as/skj-ld (over kp_cf.ld, a 32M-RAM script the crt needs)
## build the ELF, and skj-run executes it.  This is what proves a boris host can
## compile Kobold to native ColdFire from the seed alone, and the endian-neutral
## constant emission (strobj, typed .init word arrays) running big-endian.  The
## full kp-src set passes: coroutines (the ColdFire co_free seed and the argument
## liveness fix) and the transcendentals (math_free's ColdFire h_* shims) included.
check-kp-free-cf: build/skj-kpc-cf build/skj-kpc-cf build/skj-as build/skj-ld \
                  build/skj-run build/kpfree-cf/kp_crt_cf.o build/libkpaot-cf-free.a
	@MODE=free FRONTEND=build/skj-kpc-cf \
	    KPRV=build/skj-kpc-cf KPAS=build/skj-as KPLD="build/skj-ld -T kobold/kp_cf.ld" \
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
	@JIT="$(CURDIR)/build/skj-jit-arm64" RUNNER="$(A64_RUN)" \
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


# skj-cc-mips and skj-cc-mips-sf are built by the build/skj-cc-%: pattern rule
# above (cc_src_mips / cc_flags_mips and the -sf soft-float variant).

## skj-run emulator driver
#
# The two CPU cores are independent components: a project that vendors
# only one builds with EMU_COLDFIRE=0 or EMU_RV32=0 and the other core's
# sources drop out of the link.
EMU_COLDFIRE ?= 1
EMU_RV32     ?= 1
EMU_MIPS32   ?= 1
EMU_HV       ?= 1
EMU_HV_GDB   ?= 1

EMU_SRC   := emu/main.c emu/guest.c emu/hostplat.c emu/elf32.c emu/elf64.c \
             emu/procfs.c
EMU_HDR   := emu/guest.h emu/hostplat.h emu/elf32.h emu/elf64.h emu/procfs.h fs/fs.h
EMU_FLAGS := -DEMU_COLDFIRE=$(EMU_COLDFIRE) -DEMU_RV32=$(EMU_RV32) \
             -DEMU_MIPS32=$(EMU_MIPS32) -DEMU_HV=$(EMU_HV) -Ifs
## Extra link libraries an EMU backend needs (e.g. -framework Hypervisor for the
## macOS VMM).  Empty on hosts whose backends are self-contained.
EMU_LDLIBS :=
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
EMU_GDB := 1
endif
ifeq ($(EMU_MIPS32),1)
EMU_SRC += emu/mips32.c emu/mips_user.c
EMU_HDR += emu/mips32.h
## The GDB remote-protocol stub for MIPS (--gdb PORT), gated by SKJ_MIPS_GDB;
## the sibling of the RV32 stub.  A bare emu-mips vendoring omits it.
EMU_SRC += emu/mips_gdb.c
EMU_HDR += emu/mips_gdb.h
EMU_FLAGS += -DSKJ_MIPS_GDB
EMU_GDB := 1
endif
## The hardware-virtualization core (x86-64 and aarch64): the guest runs
## natively under the host hypervisor.  The VMM seam (emu/vmm.h) has one backend
## per hypervisor: Linux KVM (emu/vmm_kvm.c) and macOS Hypervisor.framework
## (emu/vmm_hvf.c), picked by the host OS.  Each backend carries a stub fallback
## on an unsupported host, so this builds everywhere and reports the VMM
## unavailable at run time.
ifeq ($(EMU_HV),1)
EMU_SRC += emu/hvcore.c
ifeq ($(HOST_IS_DARWIN),Darwin)
EMU_SRC += emu/vmm_hvf.c
## The guest runs under Hypervisor.framework; the entitlement to actually launch
## one is applied at codesign time, not here.
EMU_LDLIBS += -framework Hypervisor
else
EMU_SRC += emu/vmm_kvm.c
## The KVM backend drives a per-vcpu preemption timer (timer_create) and uses
## pthread_once/pthread_sigmask, so link pthread and rt.  -lrt is empty on glibc
## 2.34+ (timer_* folded into libc); it keeps older hosts linking.
EMU_LDLIBS += -lpthread -lrt
endif
EMU_HDR += emu/vmm.h emu/hvcore.h emu/skj_hv_abi.h
ifeq ($(EMU_HV_GDB),1)
EMU_SRC += emu/hv_gdb.c
EMU_HDR += emu/hv_gdb.h
EMU_FLAGS += -DSKJ_HV_GDB
EMU_GDB := 1
endif
endif

## The GDB stub's architecture-neutral core (packet transport, command
## dispatch, qXfer, and the breakpoint/watchpoint tables), shared by every
## per-core stub.  Linked once whenever any of the rv/mips/hv stubs is present.
ifeq ($(EMU_GDB),1)
EMU_SRC += emu/gdbstub.c
EMU_HDR += emu/gdbstub.h
endif
## The arch-neutral process sandbox + scheduler links whenever any core is
## present; each core's *_user.c is a thin binding (a skj_cpu_ops vtable over a
## void* CPU handle) that drives it.
ifneq (,$(filter 1,$(EMU_COLDFIRE) $(EMU_RV32) $(EMU_MIPS32) $(EMU_HV)))
EMU_SRC += emu/sched.c
EMU_HDR += emu/sched.h emu/run_reason.h
## The host-side 9P2000 file server the --9p doorbell drives (guest.c includes
## its header for P9_MSIZE and the server API).
EMU_SRC += fs/p9_server.c
EMU_HDR += fs/p9_server.h fs/p9_wire.h fs/ninefs.h
endif
## The shared DWARF reader links whenever any core is present, so a guest
## fault prints a symbolized backtrace (debugger item 5).  SKJ_DWARF_BACKTRACE
## gates the code in rv_user.c, cf_user.c and mips_user.c; a bare vendoring
## without dwarf/ leaves it undefined and still builds, reporting the fault pc
## alone.
ifneq (,$(filter 1,$(EMU_COLDFIRE) $(EMU_RV32) $(EMU_MIPS32)))
EMU_SRC += dwarf/dwread.c ir/arena.c
EMU_FLAGS += -DSKJ_DWARF_BACKTRACE -Idwarf -Iir
endif

build/skj-run: $(EMU_SRC) $(EMU_HDR) dwarf/dwread.h build/version.h | build
	$(CC) $(CFLAGS) $(EMU_FLAGS) -Iemu -o $@ $(EMU_SRC) -lm $(EMU_LDLIBS)

## macOS (and other non-Linux hosts): skj-run is portable C.  The host-varying
## facilities (confined opens, directory enumeration, clock, sleep, random) sit
## behind emu/hostplat.c, the one file that branches on the host OS, so the
## emulator builds with the stock host compiler (clang on macOS) and needs no
## cross toolchain or qemu.  `make macos` builds just the emulator, the tool the
## non-Linux port targets; the rest of the host toolchain (the compilers,
## assembler, linker) is host-native too and builds the same way with `make all`.
## The one path this exercises that is still UNTESTED on a real Darwin machine is
## the cap_walk confined open (see TODO.md); everything else is shared POSIX.
## The host-native tool set that builds on macOS with the stock clang and no
## cross toolchain or qemu: the front ends, the native-ISA code generator, the
## in-tree assemblers, the native in-process JIT, and the emulator.  The cross
## targets in `all` (the rv/cf/mips front ends, guest runtimes and AOT archives)
## need cross compilers macOS lacks, so they are out of this set.  The JIT and
## the AArch64 C compiler follow the host arch (NATIVE_JIT, A64_CC), so an Apple
## Silicon Mac builds the arm64 JIT, never the x86 one.
MACOS_TOOLS := build/skj-cpp build/skj-cc build/skj-cc-arm64 build/skj-pc \
               build/skj-sc build/skj-mooc \
               build/skj-as-aarch64 build/skj-as-x86-64 \
               $(NATIVE_JIT) build/skj-run
.PHONY: macos
macos: $(MACOS_TOOLS)
	@echo "built the macOS host tool set for $$(uname -s) ($(HOST_ARCH))"

## check-macos: the native validation suite for a Darwin arm64 host.  The Mach-O
## cc and libc lanes build and run native executables through the fully in-tree
## pipeline (skj-cc-arm64 -> skj-as-aarch64 -> skj-ld-aarch64); the in-process C
## and Kobold JIT lanes JIT and run arm64 code in-process, and the Kobold AOT
## lane assembles with the in-tree skj-as-aarch64 and runs native (A64_RUN is
## empty on an arm64 host, so no emulator).  Each lane skips cleanly off a Darwin
## arm64 host, so this is safe to invoke anywhere, but it is meant for the mac.
## check-kp-corpus-arm64 is NOT included: skj-kpc-arm64 currently segfaults on
## the `interp` fixture only when run native on arm64 (it passes under qemu and
## native x86-64), a latent arm64 front-end bug tracked separately.
.PHONY: check-macos
check-macos: check-cc-macho-arm64 check-libc-macho-arm64 check-jit-arm64 \
             check-kp-jit-arm64 check-kp-aot-arm64 test-macho-arm64-dwarf
	@echo "check-macos: native Darwin arm64 lanes complete"

## CI coverage for the portable confinement path: the same emulator with
## -DHP_FORCE_POSIX, which selects the cap_walk component walk in place of the
## Linux openat2 accelerator.  On Linux openat2 would otherwise hide cap_walk, so
## this is how CI compiles and exercises the path every non-Linux host relies on.
build/skj-run-posix: $(EMU_SRC) $(EMU_HDR) dwarf/dwread.h build/version.h | build
	$(CC) $(CFLAGS) $(EMU_FLAGS) -DHP_FORCE_POSIX -Iemu -o $@ $(EMU_SRC) -lm $(EMU_LDLIBS)

## VMM mechanism self-test: builds a tiny guest blob and drives it through the
## VMM backend (emu/vmm_kvm.c: long-mode bring-up plus the syscall doorbell).
## Host-native, so it runs only where the hypervisor is present; it skips
## cleanly otherwise.
build/vmm_selftest_guest.bin: emu/vmm_selftest_guest.S | build
	$(CC) -c -o build/vmm_selftest_guest.o $<
	objcopy -O binary -j .text build/vmm_selftest_guest.o $@

build/vmm_selftest: emu/vmm_selftest.c emu/vmm_kvm.c emu/vmm.h emu/skj_hv_abi.h | build
	$(CC) $(CFLAGS) -Iemu -o $@ emu/vmm_selftest.c emu/vmm_kvm.c

test-vmm: build/vmm_selftest build/vmm_selftest_guest.bin
	@./build/vmm_selftest build/vmm_selftest_guest.bin; ec=$$?; \
	 if [ $$ec -eq 42 ]; then echo "PASS: vmm selftest"; \
	 elif [ $$ec -eq 0 ]; then echo "SKIP: vmm selftest (no hypervisor)"; \
	 else echo "FAIL: vmm selftest (exit $$ec)"; exit 1; fi

## Functional-hypervisor probe for the HV test guards below.  An openable /dev/kvm
## (what `[ -r /dev/kvm ]` checks) is not enough: a host can expose the device yet
## be unable to create a vcpu (no nested virtualization), so an accessibility check
## lets those tests run and then fail spuriously.  build/vmm_selftest runs a tiny
## guest under the VMM and exits 42 only when a vcpu actually executed (0 when
## there is no hypervisor at all, another code when /dev/kvm is present but a vcpu
## cannot be created).  A guard that requires exit 42 SKIPs cleanly on a present-
## but-dead hypervisor.  Targets using vmm_works list VMM_PROBE_DEPS as prereqs so
## the probe is built before the guard runs.  The probe is x86-64-KVM specific (it
## links emu/vmm_kvm.c and its guest blob is x86-64 long-mode asm), so it gates the
## x86-64 HV tests; the aarch64-host guards keep their own uname + /dev/kvm check.
VMM_PROBE_DEPS := build/vmm_selftest build/vmm_selftest_guest.bin
vmm_works = { ./build/vmm_selftest build/vmm_selftest_guest.bin >/dev/null 2>&1; [ $$? -eq 42 ]; }

## The x86-64 hardware-virtualization guest tier: a bare-asm guest and a C guest
## (skj-cc-x86-64) that both reach the host through the doorbell syscall seam
## (runtime/syscall_x86_64_hv.S) and run under skj-run on real KVM.  The guest
## links at 1 MiB, above the long-mode page tables; linking stays external GNU
## ld, since the in-tree linker is ELF32.  Skips cleanly without /dev/kvm.
build/hv-x86_64/start.o: runtime/start_x86_64_hv.S $(X64_ASM_DEP) | build/hv-x86_64
	$(X64_ASM) $(X64_ASMFLAGS) -o $@ $<

build/hv-x86_64/syscall.o: runtime/syscall_x86_64_hv.S $(X64_ASM_DEP) | build/hv-x86_64
	$(X64_ASM) $(X64_ASMFLAGS) -o $@ $<

build/hv-x86_64/setjmp.o: runtime/setjmp_x86_64.S $(X64_ASM_DEP) | build/hv-x86_64
	$(X64_ASM) $(X64_ASMFLAGS) -o $@ $<

build/hv-x86_64/hello.elf: tests/hv/hello_x86_64.S $(X64_ASM_DEP) | build/hv-x86_64
	$(X64_ASM) $(X64_ASMFLAGS) -o build/hv-x86_64/hello.o $<
	$(X64_LD) -m elf_x86_64 -e _start -Ttext=0x100000 --build-id=none \
	    -o $@ build/hv-x86_64/hello.o

build/hv-x86_64/hello_c.elf: tests/hv/hello_c.c build/skj-cc-x86-64 \
                             build/hv-x86_64/start.o build/hv-x86_64/syscall.o \
                             $(X64_ASM_DEP) | build/hv-x86_64
	./build/skj-cc-x86-64 -o build/hv-x86_64/hello_c.s $<
	$(X64_ASM) $(X64_ASMFLAGS) -o build/hv-x86_64/hello_c.o build/hv-x86_64/hello_c.s
	$(X64_LD) -m elf_x86_64 -e _start -Ttext=0x100000 --build-id=none -o $@ \
	    build/hv-x86_64/start.o build/hv-x86_64/hello_c.o build/hv-x86_64/syscall.o

test-hv-x86-64: $(VMM_PROBE_DEPS) build/skj-run build/hv-x86_64/hello.elf build/hv-x86_64/hello_c.elf
	@if ! $(vmm_works); then echo "SKIP: hv x86-64 (no functional KVM vcpu)"; exit 0; fi; \
	 out=$$(./build/skj-run build/hv-x86_64/hello.elf 2>/dev/null); ec=$$?; \
	 if [ "$$out" != "hello hv x86" ] || [ $$ec -ne 42 ]; then \
	    echo "FAIL: hv x86-64 asm guest (out='$$out' exit=$$ec)"; exit 1; fi; \
	 out=$$(./build/skj-run build/hv-x86_64/hello_c.elf 2>/dev/null); ec=$$?; \
	 if [ "$$out" != "hello from C under HV" ] || [ $$ec -ne 7 ]; then \
	    echo "FAIL: hv x86-64 C guest (out='$$out' exit=$$ec)"; exit 1; fi; \
	 echo "PASS: hv x86-64 (asm + C guest via doorbell)"

## The aarch64 hardware-virtualization guest tier, the sibling of the x86-64
## one: the arm64 crt and the doorbell syscall seam, a C guest built by
## skj-cc-arm64.  It can only run on an aarch64 KVM host, so this verifies the
## build on any host and runs the guest only where KVM can.
build/hv-arm64/start.o: runtime/start_arm64_hv.S $(A64_ASM_DEP) | build/hv-arm64
	$(A64_AS) -o $@ $<

build/hv-arm64/syscall.o: runtime/syscall_arm64_hv.S $(A64_ASM_DEP) | build/hv-arm64
	$(A64_AS) -o $@ $<

build/hv-arm64/setjmp.o: runtime/setjmp_arm64.S $(A64_ASM_DEP) | build/hv-arm64
	$(A64_AS) -o $@ $<

build/hv-arm64/hello_c.elf: tests/hv/hello_c.c build/skj-cc-arm64 \
                            build/hv-arm64/start.o build/hv-arm64/syscall.o \
                            $(A64_ASM_DEP) | build/hv-arm64
	./build/skj-cc-arm64 -o build/hv-arm64/hello_c.s $<
	$(A64_AS) -o build/hv-arm64/hello_c.o build/hv-arm64/hello_c.s
	$(A64_LD) -e _start -Ttext=0x100000 --build-id=none -o $@ \
	    build/hv-arm64/start.o build/hv-arm64/hello_c.o build/hv-arm64/syscall.o

## The guest builds on any host; it runs only on an aarch64 host with an
## accessible /dev/kvm (the guest arch must equal the host's), so elsewhere this
## reports a build pass.
test-hv-arm64: build/skj-run build/hv-arm64/hello_c.elf
	@if [ "$$(uname -m)" = "aarch64" ] && [ -r /dev/kvm ] && [ -w /dev/kvm ]; then \
	    out=$$(./build/skj-run build/hv-arm64/hello_c.elf 2>/dev/null); ec=$$?; \
	    if [ "$$out" != "hello from C under HV" ] || [ $$ec -ne 7 ]; then \
	       echo "FAIL: hv arm64 C guest (out='$$out' exit=$$ec)"; exit 1; fi; \
	    echo "PASS: hv arm64 (C guest via doorbell)"; \
	 else echo "PASS: hv arm64 (build only; no accessible aarch64 KVM)"; fi

## HV preemption (item 11): a compute-bound guest that never traps to the
## doorbell is still preemptible.  A periodic timer breaks into KVM_RUN, so the
## round-robin scheduler keeps its turn and --max-insns bounds the run.  Without
## the timer this spin guest would hold the vcpu forever and ignore the limit;
## with it, skj-run stops at the instruction cap within a few 10 ms slices.  The
## `timeout` guard turns a regression (lost preemption) into a clean failure
## instead of a hang.  Needs /dev/kvm (an x86-64 host); skips otherwise.
test-hv-preempt: $(VMM_PROBE_DEPS) build/skj-run build/skj-cc-x86-64 build/skj-as-x86-64 \
                 build/hv-x86_64/start.o build/hv-x86_64/syscall.o
	@if ! $(vmm_works); then echo "SKIP: test-hv-preempt (no functional KVM vcpu)"; exit 0; fi; \
	 ./build/skj-cc-x86-64 -o build/hv-x86_64/spin.s tests/hv/spin.c; \
	 ./build/skj-as-x86-64 -o build/hv-x86_64/spin.o build/hv-x86_64/spin.s; \
	 $(X64_LD) -m elf_x86_64 -e _start -Ttext=0x100000 --build-id=none \
	     -o build/hv-x86_64/spin.elf build/hv-x86_64/start.o \
	     build/hv-x86_64/spin.o build/hv-x86_64/syscall.o; \
	 out=$$(timeout 10 ./build/skj-run --max-insns 40000 build/hv-x86_64/spin.elf 2>&1); ec=$$?; \
	 if [ $$ec -eq 124 ]; then \
	     echo "FAIL: hv preempt (compute-bound guest ran away; the timer did not preempt it)"; exit 1; fi; \
	 echo "$$out" | grep -q "instruction limit" || { \
	     echo "FAIL: hv preempt (no instruction-limit stop: out='$$out' ec=$$ec)"; exit 1; }; \
	 echo "PASS: hv preempt (the timer bounds a compute-bound guest; --max-insns stops it)"

## Several HV sandboxes at once, each on its own host thread: the low-level seam
## a shell server would use (one sandbox per connection).  The driver links the
## emu objects (minus the CLI main) plus the compute-bound spin guest and calls
## run_x86_64hv on NSANDBOX threads.  With a process-global preemption timer some
## sandboxes would run away and the driver would hang (caught by `timeout`); with
## the per-vcpu timer each is preempted and stops at the instruction limit.
test-hv-parallel: $(VMM_PROBE_DEPS) build/skj-run build/skj-cc-x86-64 build/skj-as-x86-64 \
                  build/hv-x86_64/start.o build/hv-x86_64/syscall.o \
                  tests/hv/parallel.c tests/hv/spin.c $(EMU_HDR) | build/hv-x86_64
	@if ! $(vmm_works); then echo "SKIP: test-hv-parallel (no functional KVM vcpu)"; exit 0; fi; \
	 ./build/skj-cc-x86-64 -o build/hv-x86_64/spin.s tests/hv/spin.c; \
	 ./build/skj-as-x86-64 -o build/hv-x86_64/spin.o build/hv-x86_64/spin.s; \
	 $(X64_LD) -m elf_x86_64 -e _start -Ttext=0x100000 --build-id=none \
	     -o build/hv-x86_64/spin.elf build/hv-x86_64/start.o \
	     build/hv-x86_64/spin.o build/hv-x86_64/syscall.o; \
	 $(CC) $(CFLAGS) $(EMU_FLAGS) -Iemu -o build/hv-x86_64/parallel \
	     tests/hv/parallel.c $(filter-out emu/main.c,$(EMU_SRC)) \
	     -lm $(EMU_LDLIBS) -lpthread; \
	 out=$$(timeout 20 ./build/hv-x86_64/parallel build/hv-x86_64/spin.elf 2>/dev/null); ec=$$?; \
	 if [ $$ec -eq 124 ]; then \
	     echo "FAIL: hv parallel (a sandbox ran away; the per-vcpu timer did not preempt it)"; exit 1; fi; \
	 case "$$out" in \
	     "parallel ok:"*) echo "PASS: hv parallel ($$out)";; \
	     *) echo "FAIL: test-hv-parallel got '$$out' (ec=$$ec)"; exit 1;; \
	 esac

## The HV GDB stub (item 10): a debugger drives one x86-64 HV guest over the
## RSP.  gdb learns the architecture from the ELF (no target.xml), so the host's
## native gdb debugs the x86-64 guest.  It exercises the whole stub subset: the
## host-side breakpoint table (`break main`), the register packet (`print $pc`),
## the memory packet (reading the guest's code bytes) and single-step `continue`
## through the doorbell MMIO to the guest's exit.  A null-deref guest triple-
## faults into a KVM shutdown (the long-mode guest installs no IDT), so there is
## no debuggable fault frame to check, unlike the emulated cores.  Needs
## /dev/kvm (so an x86-64 host) and gdb; skips cleanly otherwise.
HV_GDB ?= gdb
test-hv-gdb: $(VMM_PROBE_DEPS) build/skj-run build/skj-cc-x86-64 build/skj-as-x86-64 \
             build/hv-x86_64/start.o build/hv-x86_64/syscall.o
	@if ! $(vmm_works); then echo "SKIP: test-hv-gdb (no functional KVM vcpu)"; exit 0; fi; \
	 if ! command -v $(HV_GDB) >/dev/null 2>&1; then \
	     echo "SKIP: test-hv-gdb ($(HV_GDB) not found)"; exit 0; fi; \
	 mkdir -p build/debug; \
	 ./build/skj-cc-x86-64 -g -o build/debug/xloc.s tests/debug/loc.c; \
	 ./build/skj-as-x86-64 -o build/debug/xloc.o build/debug/xloc.s; \
	 $(X64_LD) -m elf_x86_64 -e _start -Ttext=0x100000 --build-id=none \
	     -o build/debug/xloc.elf build/hv-x86_64/start.o build/debug/xloc.o \
	     build/hv-x86_64/syscall.o; \
	 port=$$((20000 + $$$$ % 20000)); \
	 ./build/skj-run --gdb $$port build/debug/xloc.elf >/dev/null 2>build/debug/xg.err & rp=$$!; \
	 for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do \
	     grep -q listening build/debug/xg.err 2>/dev/null && break; sleep 0.2; done; \
	 $(HV_GDB) -q -nx -batch -ex "set pagination off" \
	     -ex "target remote 127.0.0.1:$$port" -ex "break main" \
	     -ex "continue" -ex "print \$$pc" -ex "x/2xb \$$pc" \
	     -ex "continue" build/debug/xloc.elf >build/debug/xg.out 2>&1; \
	 wait $$rp 2>/dev/null; \
	 ok=1; \
	 grep -qE "Breakpoint 1, 0x[0-9a-f]+ in main" build/debug/xg.out || { echo "FAIL: no break at main"; ok=0; }; \
	 grep -q "<main+" build/debug/xg.out || { echo "FAIL: register packet wrong pc"; ok=0; }; \
	 grep -qE "<main\+[0-9]+>:[[:space:]]+0x53" build/debug/xg.out || { echo "FAIL: memory packet did not read guest code"; ok=0; }; \
	 grep -q "exited with code 05" build/debug/xg.out || { echo "FAIL: loc did not exit 05"; ok=0; }; \
	 if [ $$ok = 1 ]; then \
	     echo "PASS: gdb drives the skj-run HV x86-64 RSP stub (break, regs, mem, exit)"; \
	 else cat build/debug/xg.out; exit 1; fi

## FP (SSE) registers and a write watchpoint over the HV x86-64 stub: an xmm
## register round-trips through g/G, and a Z2 watchpoint on a global stops the
## guest at the write (snapshot/compare while single-stepping the vcpu).
test-hv-gdb-fpwatch: $(VMM_PROBE_DEPS) build/skj-run build/skj-cc-x86-64 build/skj-as-x86-64 \
                     build/hv-x86_64/start.o build/hv-x86_64/syscall.o
	@if ! $(vmm_works); then echo "SKIP: test-hv-gdb-fpwatch (no functional KVM vcpu)"; exit 0; fi; \
	 if ! command -v $(HV_GDB) >/dev/null 2>&1; then \
	     echo "SKIP: test-hv-gdb-fpwatch ($(HV_GDB) not found)"; exit 0; fi; \
	 mkdir -p build/debug; \
	 ./build/skj-cc-x86-64 -g -o build/debug/xwatch.s tests/debug/watch.c; \
	 ./build/skj-as-x86-64 -o build/debug/xwatch.o build/debug/xwatch.s; \
	 $(X64_LD) -m elf_x86_64 -e _start -Ttext=0x100000 --build-id=none \
	     -o build/debug/xwatch.elf build/hv-x86_64/start.o build/debug/xwatch.o \
	     build/hv-x86_64/syscall.o; \
	 port=$$((20000 + ($$$$ + 4) % 20000)); \
	 ./build/skj-run --gdb $$port build/debug/xwatch.elf >/dev/null 2>build/debug/xw.err & rp=$$!; \
	 for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do \
	     grep -q listening build/debug/xw.err 2>/dev/null && break; sleep 0.2; done; \
	 $(HV_GDB) -q -nx -batch -ex "set pagination off" \
	     -ex "target remote 127.0.0.1:$$port" -ex "break main" -ex "continue" \
	     -ex "set \$$xmm0.v2_int64[0] = 42" -ex "print \$$xmm0.v2_int64[0]" \
	     -ex "watch *(int *)&gcounter" -ex "continue" build/debug/xwatch.elf \
	     >build/debug/xw.out 2>&1; \
	 kill $$rp 2>/dev/null; wait $$rp 2>/dev/null; \
	 ok=1; \
	 grep -q "= 42" build/debug/xw.out || { echo "FAIL: xmm register did not round-trip"; ok=0; }; \
	 grep -qiE "watchpoint" build/debug/xw.out || { echo "FAIL: watchpoint not set"; ok=0; }; \
	 grep -q "New value = 1" build/debug/xw.out || { echo "FAIL: watchpoint did not fire on write"; ok=0; }; \
	 if [ $$ok = 1 ]; then \
	     echo "PASS: HV x86-64 stub serves SSE registers and a write watchpoint"; \
	 else cat build/debug/xw.out; exit 1; fi

## Native macOS (Mach-O) target smoke (0.29.0 strand 3, Phase 1).  skj-cc-arm64
## emits Mach-O-assemblable AArch64 (the Darwin flavor: _-prefixed symbols, Mach-O
## sections, PC-relative symbol addresses, stack-passed variadic arguments), and
## Apple clang assembles and links it against libSystem into a native executable.
## On a Darwin arm64 host it builds and runs the program; elsewhere (CI runs on
## Linux) it forces the Mach-O flavor with -mmacho and checks the framing, so the
## emitter path is exercised without a mac.  MACHO_CC is the host C toolchain that
## drives the assembler and linker (clang).
MACHO_CC ?= clang
test-macho-arm64: build/skj-cc-arm64 build/skj-cc-arm64-macho build/skj-as-aarch64 build/skj-ld-aarch64 tests/macho/hello.c | build
	@if [ "$$(uname -s)" = "Darwin" ] && [ "$$(uname -m)" = "arm64" ]; then \
	    ./build/skj-cc-arm64 -o build/hello_macho.s tests/macho/hello.c; \
	    ./build/skj-as-aarch64 -o build/hello_macho.o build/hello_macho.s; \
	    ./build/skj-ld-aarch64 -o build/hello_macho build/hello_macho.o; \
	    out=$$(./build/hello_macho); ec=$$?; \
	    if [ "$$out" != "hello macho 42" ] || [ $$ec -ne 0 ]; then \
	       echo "FAIL: macho arm64 (out='$$out' exit=$$ec)"; exit 1; fi; \
	    echo "PASS: macho arm64 (skj-cc-arm64 -> skj-as-aarch64 -> skj-ld-aarch64 -> native run)"; \
	 else \
	    ./build/skj-cc-arm64-macho -o build/hello_macho.s tests/macho/hello.c; \
	    if grep -q '^_main:' build/hello_macho.s && \
	       grep -q '@PAGE' build/hello_macho.s && \
	       ! grep -q 'note.GNU-stack' build/hello_macho.s; then \
	        echo "PASS: macho arm64 (Mach-O generation only; not a Darwin arm64 host)"; \
	    else echo "FAIL: macho arm64 generation framing wrong"; exit 1; fi; \
	 fi

## skj-cc-arm64 -g carries a DWARF line table through the whole Mach-O pipeline:
## the compiler emits .file/.loc, skj-as-aarch64 assembles them into __DWARF
## (__debug_line/info/abbrev) with text-base relocations, and skj-ld-aarch64
## resolves those and places __DWARF between __DATA and __LINKEDIT (inside the
## signed region).  Structure is validated with llvm-dwarfdump --verify on both
## the object and the executable; this runs on any host (a Mach-O binary need not
## run here, only parse), so it guards only on llvm-dwarfdump being present.
## Source-level stepping in lldb is verified separately on a Darwin arm64 host.
test-macho-arm64-dwarf: build/skj-cc-arm64-macho build/skj-as-aarch64-macho \
                        build/skj-ld-aarch64 tests/macho/hello.c \
                        tests/macho/dwmulti.c tests/macho/dwinc.h | build
	@DD=`command -v llvm-dwarfdump || command -v dwarfdump || true`; \
	 if [ -z "$$DD" ]; then \
	    echo "SKIP: test-macho-arm64-dwarf (no dwarfdump)"; exit 0; fi; \
	 mkdir -p build/debug; \
	 ./build/skj-cc-arm64-macho -g -o build/debug/mdw.s tests/macho/hello.c && \
	 ./build/skj-as-aarch64-macho -o build/debug/mdw.o build/debug/mdw.s && \
	 ./build/skj-ld-aarch64 -o build/debug/mdw build/debug/mdw.o \
	    || { echo "FAIL: -g Mach-O pipeline"; exit 1; }; \
	 for f in build/debug/mdw.o build/debug/mdw; do \
	     $$DD --debug-line $$f 2>/dev/null | grep -qE '^0x[0-9a-f]+ +[0-9]+ ' \
	         || { echo "FAIL: $$f has no __debug_line rows"; exit 1; }; \
	     $$DD --verify $$f 2>&1 | grep -q 'No errors\.' \
	         || { echo "FAIL: $$f DWARF does not verify"; exit 1; }; \
	 done; \
	 echo "PASS: macho arm64 -g (__DWARF assembled and linked through, verifies)"; \
	 ./build/skj-cc-arm64-macho -g -o build/debug/mdwm.s tests/macho/dwmulti.c && \
	 ./build/skj-as-aarch64-macho -o build/debug/mdwm.o build/debug/mdwm.s && \
	 ./build/skj-ld-aarch64 -o build/debug/mdwm build/debug/mdwm.o \
	    || { echo "FAIL: -g multi-file pipeline"; exit 1; }; \
	 $$DD --debug-line build/debug/mdwm 2>/dev/null | grep -q 'file_names\[  2\]' \
	     || { echo "FAIL: multi-file line table missing a second file"; exit 1; }; \
	 $$DD --debug-line build/debug/mdwm 2>/dev/null \
	     | awk '/ is_stmt/ && $$4 == 2 { f2=1 } /is_stmt/ && $$3+0 > 0 { c=1 } END { exit !(f2 && c) }' \
	     || { echo "FAIL: no row names file 2 with a column"; exit 1; }; \
	 $$DD --verify build/debug/mdwm 2>&1 | grep -q 'No errors\.' \
	     || { echo "FAIL: multi-file DWARF does not verify"; exit 1; }; \
	 echo "PASS: macho arm64 -g multi-file (header code attributes to file 2, columns present)"

## The native macOS (Mach-O) corpus lanes: compile each cc/libc test with
## skj-cc-arm64 (Mach-O), assemble and link with Apple clang against libSystem,
## and run natively.  These exercise the Mach-O code generation across the whole
## corpus (the compiler, not the in-tree libc, which is not ported to Darwin
## syscalls; libSystem is the C library).  They run only on a Darwin arm64 host;
## a Mach-O binary cannot run elsewhere, so off a mac they skip.
##
## MACHO_CC_EXCLUDE / MACHO_LIBC_TESTS carry the corpus tailoring: a few programs
## depend on a C-library detail that differs from libSystem (a struct layout such
## as FILE, or an in-tree-libc or Darwin-unsupported feature) and are named here.
## The libc lane links libSystem for the C library, not the in-tree libc, so it
## runs only the few phase programs that assert standard C behavior and reference
## no in-tree internals (see MACHO_LIBC_TESTS below for the selection rationale).
# The Darwin variadic ABI is implemented on both sides: the caller passes the
# variadic arguments on the stack, and a variadic callee's va_start walks that
# stack through the va_list.  So the whole cc corpus runs, including the programs
# that define variadic functions.  The lane links runtime/va_aarch64.c, whose
# __va_arg those programs call (its stack path serves the Darwin walk).
MACHO_CC_EXCLUDE ?=
# The libc corpus is written against the in-tree libc, so most of it is not
# portable to libSystem and only the phases that assert standard C behavior and
# reference no in-tree internals run here.  Phase e covers the snprintf float
# conversions (%f/%e/%g) and phase g covers atexit ordering and exit-time flush;
# both call libSystem through the Darwin variadic ABI and produce output a
# standard C library matches.  The rest are excluded on purpose: phases a and b
# assert implementation-defined choices the in-tree libc makes but libSystem does
# not (malloc(0)/realloc(p,0) return NULL, %p prints "(nil)"); phase f pins the
# in-tree rand sequence and strerror strings; phases c and h reference in-tree
# symbols absent from libSystem (__libc_stdin/__libc_stdout, __libc_malloc_live);
# and the FILE/fs/threads/coroutine phases are in-tree-only as well.
MACHO_LIBC_TESTS ?= tests/libc_phase_e.c tests/libc_phase_g.c
check-cc-macho-arm64: build/skj-cc-arm64 build/skj-as-aarch64 build/skj-ld-aarch64 runtime/va_aarch64.c | build
	@if [ "$$(uname -s)" != "Darwin" ] || [ "$$(uname -m)" != "arm64" ]; then \
	    echo "SKIP: check-cc-macho-arm64 (not a Darwin arm64 host)"; exit 0; fi; \
	 mkdir -p "$(CURDIR)/build/macho-cc"; \
	 ./build/skj-cc-arm64 -o "$(CURDIR)/build/macho-cc/va.s" runtime/va_aarch64.c; \
	 ./build/skj-as-aarch64 -o "$(CURDIR)/build/macho-cc/va.o" \
	    "$(CURDIR)/build/macho-cc/va.s"; \
	 CCBIN="$(CURDIR)/build/skj-cc-arm64" \
	 ASM="$(CURDIR)/build/skj-as-aarch64 -o" LD="$(CURDIR)/build/skj-ld-aarch64 -o" \
	 START="" HALF="$(CURDIR)/build/macho-cc/va.o" QEMU="" \
	 OUTDIR="$(CURDIR)/build/macho-cc" \
	 EXCLUDE="$(MACHO_CC_EXCLUDE)" sh tests/run-cc-tests.sh

check-libc-macho-arm64: build/skj-cc-arm64 build/skj-as-aarch64 build/skj-ld-aarch64 | build
	@if [ "$$(uname -s)" != "Darwin" ] || [ "$$(uname -m)" != "arm64" ]; then \
	    echo "SKIP: check-libc-macho-arm64 (not a Darwin arm64 host)"; exit 0; fi; \
	 CCBIN="$(CURDIR)/build/skj-cc-arm64" \
	 ASM="$(CURDIR)/build/skj-as-aarch64 -o" LD="$(CURDIR)/build/skj-ld-aarch64 -o" \
	 START="" HALF="" QEMU="" LIBC=" " CFLAGS="-DSKJ_LIBC_FLOAT" \
	 TEST="$(addprefix $(CURDIR)/,$(MACHO_LIBC_TESTS))" \
	 OUTDIR="$(CURDIR)/build/macho-libc" sh tests/run-libc-tests.sh

## ColdFire assembler
AS_SRC := as/main.c as/asm_lex.c as/asm_obj.c as/parse.c as/encode.c as/elf.c \
          dwarf/dwarf.c ir/util.c ir/arena.c

build/skj-as: $(AS_SRC) as/as.h as/asm_lex.h as/asm_obj.h dwarf/dwarf.h | build
	$(CC) $(CFLAGS) -Ias -Iir -Idwarf -o $@ $(AS_SRC)

## RISC-V RV32 assembler (shares the lexer + object helpers with skj-as)
AS_RV_SRC := as/rv_main.c as/asm_lex.c as/asm_obj.c as/rv_parse.c \
             as/rv_encode.c as/rv_macro.c as/rv_elf.c dwarf/dwarf.c \
             ir/util.c ir/arena.c

## MIPS I assembler (shares the lexer + object helpers with skj-as)
## Built up in steps; step 2 encodes the instruction set.  See doc/tools/mips.md.
AS_MIPS_SRC := as/mips_main.c as/asm_lex.c as/asm_obj.c as/mips_parse.c \
               as/mips_encode.c as/mips_elf.c as/mips_sched.c dwarf/dwarf.c \
               ir/util.c ir/arena.c

## x86-64 assembler (shares the lexer + object helpers with skj-as).  Reads the
## GNU .intel_syntax noprefix the x86-64 backends emit; no DWARF (the bounded
## input has no .loc).
AS_X64_SRC := as/x86_64_main.c as/asm_lex.c as/asm_obj.c as/x86_64_parse.c \
              as/x86_64_encode.c as/x86_64_elf.c \
              ir/util.c ir/arena.c

## AArch64 assembler (shares the lexer + object helpers with skj-as).  Reads the
## native GNU as syntax the arm64 backends emit.  On the Mach-O (Darwin) path it
## also consumes .file/.loc and emits __DWARF line info, so it links dwarf/.
AS_A64_SRC := as/aarch64_main.c as/asm_lex.c as/asm_obj.c as/aarch64_parse.c \
              as/aarch64_encode.c as/aarch64_elf.c as/aarch64_macho.c \
              dwarf/dwarf.c ir/util.c ir/arena.c

# Every cross assembler shares the lexer and object helpers and compiles with
# -Ias -Iir; the ISAs that carry DWARF line info add -Idwarf, and the Mach-O
# variant adds -DSKJ_DARWIN.  Key the source list and the extra flags by the
# tool-name suffix and build them all with one pattern rule.  aarch64 is the
# ELF assembler, aarch64-macho the same sources fixed to the Darwin/Mach-O
# output (-DSKJ_DARWIN) for cross use on a non-Darwin host.  The x86-64
# assembler carries no DWARF (its bounded input has no .loc).  The bare
# build/skj-as (ColdFire) keeps its own rule.
as_src_rv             := $(AS_RV_SRC)
as_src_mips           := $(AS_MIPS_SRC)
as_src_x86-64         := $(AS_X64_SRC)
as_src_aarch64        := $(AS_A64_SRC)
as_src_aarch64-macho  := $(AS_A64_SRC)
as_flags_rv            := -Idwarf
as_flags_mips          := -Idwarf
as_flags_aarch64       := -Idwarf
as_flags_aarch64-macho := -Idwarf -DSKJ_DARWIN

build/skj-as-%: $$(as_src_$$*) as/asm_lex.h as/asm_obj.h $(wildcard as/*.h) $(wildcard dwarf/*.h) | build
	$(CC) $(CFLAGS) $(as_flags_$*) -Ias -Iir -o $@ $(as_src_$*)

## skj-ld linker
LD_SRC := ld/main.c ld/elf_read.c ld/script.c ld/link.c ld/elf_write.c ld/mapfile.c ld/archive.c ir/util.c ir/arena.c

build/skj-ld: $(LD_SRC) ld/ld.h ld/mapfile.h | build
	$(CC) $(CFLAGS) -Ild -Iir -o $@ $(LD_SRC)

## RISC-V RV32 linker (shares layout core, script parser and mapfile reader)
LD_RV_SRC := ld/rv_main.c ld/rv_elf_read.c ld/rv_link.c ld/rv_elf_write.c \
             ld/rv_archive.c ld/link.c ld/script.c ld/mapfile.c \
             ir/util.c ir/arena.c

## MIPS I linker (shares layout core, script parser and mapfile reader).
## REL relocations with the o32 HI16/LO16 addend carry-pairing.  See doc/tools/mips.md.
LD_MIPS_SRC := ld/mips_main.c ld/mips_elf_read.c ld/mips_link.c \
               ld/mips_elf_write.c ld/mips_psexe_write.c ld/mips_archive.c \
               ld/link.c ld/script.c ld/mapfile.c ir/util.c ir/arena.c

## x86-64 ELF64 linker (shares the layout core, script parser and mapfile
## reader; the ELF64 reader/writer are shared with the AArch64 linker).
LD_X64_SRC := ld/x86_64_main.c ld/elf64_read.c ld/x86_64_link.c \
              ld/elf64_write.c ld/elf64_archive.c ld/link.c ld/script.c \
              ld/mapfile.c ir/util.c ir/arena.c

## AArch64 ELF64 linker (shares the ELF64 core with the x86-64 linker; only the
## machine id and the relocation set differ).
LD_A64_SRC := ld/aarch64_main.c ld/elf64_read.c ld/aarch64_link.c \
              ld/elf64_write.c ld/elf64_archive.c ld/link.c ld/script.c \
              ld/mapfile.c ir/util.c ir/arena.c

## Mach-O linker for the Darwin arm64 target: reads the MH_OBJECT relocatables
## skj-as-aarch64 writes, binds libSystem imports through stubs and a __got, and
## self-signs ad-hoc.  A fresh codebase (the ELF linkers share nothing with it).
LD_MACHO_SRC := ld/macho_main.c ld/macho_read.c ld/macho_link.c \
                ld/macho_sha256.c ir/util.c ir/arena.c

# Every cross linker compiles the same way (-Ild -Iir); only its source list
# differs.  Key those by the tool-name suffix and build them all with one
# pattern rule.  arm64 is the ELF64 linker (LD_A64_SRC), aarch64 the Mach-O one
# (LD_MACHO_SRC); the two targets keep distinct suffixes.  ld headers are a
# wildcard prerequisite.  The bare build/skj-ld (ColdFire) keeps its own rule.
ld_src_rv      := $(LD_RV_SRC)
ld_src_mips    := $(LD_MIPS_SRC)
ld_src_x86-64  := $(LD_X64_SRC)
ld_src_arm64   := $(LD_A64_SRC)
ld_src_aarch64 := $(LD_MACHO_SRC)

build/skj-ld-%: $$(ld_src_$$*) $(wildcard ld/*.h) | build
	$(CC) $(CFLAGS) -Ild -Iir -o $@ $(ld_src_$*)

## skj-ar: a minimal ar(1) archive tool (create with symbol index, list,
## extract).  Generic over the object machine and byte order, so one tool
## serves every target's linker.
AR_SRC := ar/main.c ld/mapfile.c ir/util.c ir/arena.c

build/skj-ar: $(AR_SRC) ld/mapfile.h ir/util.h ir/arena.h | build
	$(CC) $(CFLAGS) -Ild -Iir -o $@ $(AR_SRC)

# Per-test rules: tests/<name>.scm -> build/<name>.s -> build/<name>.o
#                 + build/start.o -> build/<name> (scheme; see build/scm_%.s)
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

# Every build/<subdir> below is used only as an order-only prerequisite and is
# created the same way.  One multi-target rule replaces the two-line mkdir rule
# each used to carry.  Add a new scratch subdir to this list, not as its own
# rule.  (build itself stays separate above: much of the tree hangs off it.)
BUILD_DIRS := build/kpfree-rv build/kpfree-cf build/hv-x86_64 build/hv-arm64 \
	build/rv build/mips build/cf build/san build/arm64 build/x86_64 build/rv32 \
	build/rv-skj build/sandbox build/spawn-argv build/spawn-wait build/spawn-kill build/spawn-backtrace \
	build/env-test build/xlang-sandbox build/sandbox-mips build/sandbox-cf \
	build/mips-nr build/mips-asm build/cf-asm build/rv-asm build/x86-asm build/arm64-asm \
	build/mips/libpsx build/psx build/psx-units
$(BUILD_DIRS):
	mkdir -p $@

build/version.h: FORCE | build
	@printf '#ifndef SKJ_VERSION_H\n#define SKJ_VERSION_H\n#define SKJ_VERSION "%s"\n#endif\n' '$(VERSION)' > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp

# Every tool's main.c includes "version.h".  This covers the tools outside
# $(TOOLS) too, since they are built by their own targets, not by `make all`.
# The skj-pc LP64 host variants are such tools: they are not in $(TOOLS) but
# build/skj-kpc-rv, -cf, and -arm64 are built from build/skj-pc-x86-64, so a
# bootstrap that starts from a kpcom seed reaches them without `make all` first.
VERSIONED := $(TOOLS) build/skj-sc-x86-64 \
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

check: build/skj-mooc build/skj-pc $(SCM_TESTS:tests/%.scm=build/%) $(MOO_TESTS:tests/%.moo=build/%) $(MOO_TOY_TESTS:tests/%.moo=build/%) $(PAS_TESTS_NOFLOAT:tests/%.pas=build/%)
	@sh tests/run-tests.sh "$(QEMU)"

## RISC-V per-test rules: tests/<name>.scm -> build/rv/<name>.s -> .o -> binary
RV_SCM_TESTS := $(SCM_TESTS)

build/rv/%.o: build/rv/%.s
	$(RV_AS) -march=rv32im -mabi=ilp32 -o $@ $<

build/rv/start.o: runtime/start_rv.S runtime/crt_entry_rv.S runtime/syscall_rv.S runtime/i64_rv.S | build/rv
	$(RV_AS) -Iruntime -march=rv32im -mabi=ilp32 -o $@ $<

build/rv/%: build/rv/%.o build/rv/start.o
	$(RV_LD) -m elf32lriscv -o $@ build/rv/start.o $<

check-rv: build/skj-sc-rv $(RV_SCM_TESTS:tests/%.scm=build/rv/%)
	@sh tests/run-tests.sh "$(QEMU_RV)" build/rv

## Run an ELF under qemu-riscv32 configured to match the rv32.c emulator's ISA.
## Usage: make run-rv ELF=build/rv/foo [ARGS='...']
run-rv:
	@test -n "$(ELF)" || { echo "usage: make run-rv ELF=<path> [ARGS='...']" >&2; exit 2; }
	$(QEMU_RV) -cpu $(QEMU_RV_CPU) $(ELF) $(ARGS)

## MIPS I per-test rules: tests/<name>.scm -> build/mips/<name>.s -> .o -> binary.
## The toolchain (mipsel-none-elf) defaults to -march=mips1, o32,
## little-endian; the generated asm leans on the assembler's default
## `.set reorder` to fill the load/branch delay slots for R2000/R3000.
MIPS_SCM_TESTS := $(SCM_TESTS)

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

check-mips: build/skj-sc-mips $(MIPS_SCM_TESTS:tests/%.scm=build/mips/%)
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
FS_SRC := fs/fs.c fs/ramfs.c fs/hostfs.c fs/devfs.c fs/romfs.c fs/romfs_build.c \
          fs/ninefs.c fs/p9_server.c
FS_HDR := fs/fs.h fs/fs_internal.h fs/ramfs.h fs/hostfs.h fs/ambientfs.h \
          fs/hostcap.h fs/devfs.h fs/romfs.h fs/ninefs.h fs/p9_server.h fs/p9_wire.h

build/test_fs: tests/fs_test.c $(FS_SRC) $(FS_HDR) | build
	$(CC) $(CFLAGS) -DFS_WITH_HOSTFS -DFS_WITH_DEV -DFS_WITH_ROM -DFS_WITH_9P -DFS_WITH_RAM -Ifs -o $@ tests/fs_test.c $(FS_SRC)

test-fs: build/test_fs
	@./build/test_fs

## skj-mkromfs: the host packer that turns a directory into a romfs image.
build/skj-mkromfs: fs/mkromfs.c fs/romfs_build.c fs/romfs.h fs/fs.h | build
	$(CC) $(CFLAGS) -Ifs -o $@ fs/mkromfs.c fs/romfs_build.c

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
FS_FREE_CC := ./build/skj-cc-rv -Iinclude -Ilibc -Ifs -DFS_WITH_AMBIENT -DFS_WITH_HOSTCAP -DFS_AMBIENT_DEFAULT -DFS_WITH_DEV -DFS_WITH_ROM -DFS_WITH_9P -DFS_WITH_STREAMS -DFS_WITH_RAM
# The fs/ layer objects for the rv freestanding archive all build the same way:
# one source per target stem, compiled with the full FS feature set and
# assembled with skj-as-rv.  One pattern rule covers them (the generic
# kobold/%.c rule above does not match, since fs/ and kobold/ share no basename).
build/kpfree-rv/%.o: fs/%.c $(FS_HDR) $(wildcard include/*.h) build/skj-cc-rv build/skj-as-rv Makefile | build/kpfree-rv
	$(FS_FREE_CC) -o $(@:.o=.s) $<
	./build/skj-as-rv -o $@ $(@:.o=.s)

check-fs-free: build/kpfree-rv/fs.o build/kpfree-rv/ramfs.o build/kpfree-rv/ambientfs.o build/kpfree-rv/hostcap.o build/kpfree-rv/devfs.o build/kpfree-rv/romfs.o build/kpfree-rv/ninefs.o build/kpfree-rv/p9_guest.o
	@echo "PASS: fs/ (fs.c + ramfs.c + ambientfs.c + hostcap.c + devfs.c + romfs.c + ninefs.c + p9_guest.c) builds freestanding for rv32"

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

build/cf/%.o: runtime/%.c | build/cf
	$(M68K_CC) $(CF_RT_CFLAGS) -c -o $@ $<

CF_MOO_RT     := build/start.o build/cf/str.o build/cf/list.o build/cf/host_stub.o
CF_MOO_TOY_RT := build/start.o build/cf/str.o build/cf/list.o build/cf/toy_host.o

CF_SCM_BINS := $(SCM_TESTS:tests/%.scm=build/cf/%)
CF_MOO_BINS := $(MOO_TESTS:tests/%.moo=build/cf/%)
CF_TOY_BINS := $(MOO_TOY_TESTS:tests/%.moo=build/cf/%)
CF_PAS_BINS := $(PAS_TESTS_NOFLOAT:tests/%.pas=build/cf/%)

$(CF_SCM_BINS): build/cf/%: build/%.o build/start.o build/skj-ld | build/cf
	./build/skj-ld -o $@ build/start.o $<

$(CF_MOO_BINS): build/cf/%: build/%.o $(CF_MOO_RT) build/skj-ld | build/cf
	./build/skj-ld -o $@ $(CF_MOO_RT) $<

$(CF_TOY_BINS): build/cf/%: build/%.o $(CF_MOO_TOY_RT) build/skj-ld | build/cf
	./build/skj-ld -o $@ $(CF_MOO_TOY_RT) $<

$(CF_PAS_BINS): build/cf/%: build/%.o build/start.o build/cf/pascal_rt.o build/skj-ld | build/cf
	./build/skj-ld -o $@ build/start.o build/cf/pascal_rt.o $<

check-emu: build/skj-run build/skj-mooc build/skj-pc \
           $(CF_SCM_BINS) $(CF_MOO_BINS) $(CF_TOY_BINS) $(CF_PAS_BINS)
	@sh tests/run-tests.sh ./build/skj-run build/cf

## The RISC-V tests link no cross-compiled C, so the same binaries the
## qemu tier runs go straight to the emulator.
check-rv-emu: build/skj-run build/skj-sc-rv \
              $(RV_SCM_TESTS:tests/%.scm=build/rv/%)
	@sh tests/run-tests.sh ./build/skj-run build/rv

## The MIPS tests link no cross-compiled C, so the same binaries the qemu tier
## runs go straight to the in-tree emulator (the mips32 user-mode core).
check-mips-emu: build/skj-run build/skj-sc-mips \
                $(MIPS_SCM_TESTS:tests/%.scm=build/mips/%)
	@sh tests/run-tests.sh ./build/skj-run build/mips

## The cooperative blocking-read path: an echo guest fed buffered and delayed
## stdin on every sandbox core, proving read() parks and re-runs correctly (see
## tests/run-coop-read.sh).  The guest is a small C program built through the
## skjcc driver for each core; its build rules sit beside the DRIVER_LIB_*
## definitions (below) so their prerequisite lists are non-empty when read.
test-coop-read: build/skj-run build/coop_echo build/rv/coop_echo build/mips/coop_echo
	@sh tests/run-coop-read.sh ./build/skj-run \
	    cf=build/coop_echo rv=build/rv/coop_echo mips=build/mips/coop_echo

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
BENCH_CF_DEPS := $(CF_SCM_BINS) $(CF_MOO_BINS) $(CF_TOY_BINS) \
                 $(CF_PAS_BINS)
BENCH_RV_DEPS := build/skj-sc-rv \
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
check-emu-all: check-emu check-rv-emu check-mips-emu test-coop-read \
               test-rv-irq test-rv-run test-guest-mmap \
               test-rv-sandbox test-rv-xlang-sandbox test-mips-sandbox \
               test-cf-sandbox test-rv-spawn-argv test-rv-spawn-wait test-rv-spawn-kill \
               test-rv-spawn-backtrace \
               test-rv-spawn-mounts test-cf-spawn-mounts \
               test-rv-spawn-pipe test-cf-spawn-pipe \
               test-rv-jsh test-cf-jsh test-mips-jsh test-x86-64-jsh test-arm64-jsh \
               test-rv-ps \
               test-rv-vdso test-cf-vdso test-mips-vdso \
               test-rv-mem-root test-sched-embed \
               test-hv-x86-64-vdso test-hv-arm64-vdso test-hv-parallel test-hv-x86-64-pump \
               test-rv-env test-cf-env test-hv-x86-64-env test-rv-decode-cache \
               test-rv-expand test-rv-bus test-rv-csr test-rv-decode test-rv-fp \
               check-rv32 \
               test-fpu-emu test-i64-emu test-ops-emu bench \
               check-jit-arm64 check-jit-rv check-jit-arm64-disasm check-jit-rv-disasm \
               test-jit-trap-arm64 test-jit-arm64-high \
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
build/san/test_rv_run: tests/test_rv_run.c emu/rv32.c emu/rv32.h | build build/san
	$(CC) $(CFLAGS) $(SAN_CFLAGS) -Iemu -o $@ tests/test_rv_run.c emu/rv32.c -lm

build/san/test_guest_mmap: tests/test_guest_mmap.c emu/guest.c emu/guest.h \
                           emu/procfs.c emu/procfs.h \
                           emu/hostplat.c emu/hostplat.h emu/elf32.c emu/elf32.h \
                           emu/elf64.c emu/elf64.h fs/p9_server.c fs/p9_server.h | build build/san
	$(CC) $(CFLAGS) $(SAN_CFLAGS) -Iemu -Ifs -o $@ tests/test_guest_mmap.c \
	    emu/guest.c emu/procfs.c emu/hostplat.c emu/elf32.c emu/elf64.c fs/p9_server.c -lm

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
	$(CC) $(CFLAGS) $(SAN_CFLAGS) $(EMU_FLAGS) -Iemu -o $@ $(EMU_SRC) -lm $(EMU_LDLIBS)

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

## The sanitizer build of the single-step skj-kpc.  check-san-kp feeds it prebuilt
## .kpm modules, so only the C .kpm-to-assembly path runs under the sanitizer (the
## Pascal front-end object is linked but not exercised); this sanitizes the KPM
## reader (kobold/kpm.c) and the emit pipeline the way the retired san/skj-kp did.
build/san/skj-kpc: $(HOST_PC) tests/pascal_kpcom_host.c build/pascal_rt_host.o $(KPCOM_SRCS) \
                   kobold/kpbuild.c kobold/kpb_fe.c kobold/kpb_native.c kobold/kp_aot.c kobold/kpm.c \
                   kobold/kpm_write.c $(IR_SRC) $(BE_X64_SEL) \
                   kobold/kpbuild.h kobold/kpm.h kobold/kp_aot.h build/version.h $(HOST_ASM_DEP) | build build/san
	$(HOST_PC) -d KOBOLD -d KP_TARGET_X64 -d KP_ONESTEP -o build/san/kpcom-x86-64-fe.s $(KPCOM_SRC_PAS)
	$(call host_assemble,build/san/kpcom-x86-64-fe.s,build/san/kpcom-x86-64-fe.o)
	$(CC) $(CFLAGS) $(SAN_CFLAGS) $(HOST_NO_PIE) -DX86_BITS=64 -Iir -Ibackend -Ikobold \
	    build/san/kpcom-x86-64-fe.o tests/pascal_kpcom_host.c build/pascal_rt_host.o \
	    kobold/kpbuild.c kobold/kpb_fe.c kobold/kpb_native.c kobold/kp_aot.c kobold/kpm.c kobold/kpm_write.c \
	    $(IR_SRC) $(BE_X64_SEL) -lm -o $@

# The KPM1 reader-pipeline fuzz harness (kpm_read -> regalloc -> target_emit).
# This path maps no executable memory, so full ASan applies beside UBSan.
build/san/kpm-fuzz: kobold/kpm_fuzz.c kobold/kpm.c $(IR_SRC) $(BE_X64_SEL) kobold/kpm.h kobold/kp_aot.h ir/ir.h \
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
                           dwarf/dwarf.h tests/fuzz_mutate.h build/version.h | build build/san
	$(CC) $(CFLAGS) $(SAN_CFLAGS) -D_GNU_SOURCE -DASFUZZ_AARCH64 -Ias -Iir -Idwarf -Itests -o $@ \
	    as/as_fuzz.c $(AS_A64_FUZZ_LIB)

# The IR front-end parser fuzz harnesses (one arch-parameterized source over
# each front end's library, main.c dropped).  Lexer + parser + lowering, source
# in, assembly out (discarded), so full ASan applies.  MOO_SRC and PC_SRC are
# defined later than this rule, so the pascal and moo prerequisites list the
# sources explicitly (the deferred-definition hazard noted on the san rules).
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

# The remaining front ends (scheme, moo, Pascal) and the archive tool
# also take full ASan+UBSan; each is probed compile-only by run-compile-san.sh.
# SC_SRC,
# MOO_SRC, and PC_SRC are defined later in the Makefile than these rules, so a
# prerequisite of them would expand empty at read time and never trigger a
# rebuild (a deferred-definition hazard);
# the source files are listed explicitly in the prerequisites here, while the
# recipe's deferred $(VAR) expansion is fine.  AR_SRC is defined
# before this point, so those rules use the variable directly.
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
build/san/skj-kp-jit: kobold/jit_main.c kobold/frontend.c build/x86-64/kpcom_kpm.c kobold/kpbuild.c kobold/kpm_write.c $(KP_RT) build/kp_co_lp64.o kobold/frontend.h kobold/kpbuild.h $(KP_HDRS) build/version.h Makefile | build build/san
	$(CC) $(KP_CFLAGS) $(KP_FS_FLAGS) -Ibuild/x86-64 $(SAN_CFLAGS) -Ibuild -o $@ kobold/jit_main.c kobold/frontend.c build/x86-64/kpcom_kpm.c kobold/kpbuild.c kobold/kpm_write.c $(KP_RT) build/kp_co_lp64.o -lm

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

check-san-kp: build/san/skj-kpc
	@echo "== check-san: KPM1 reader (skj-kpc) =="
	@$(SAN_RUNENV) KP="$(CURDIR)/build/san/skj-kpc" sh tests/run-kp-tests.sh

check-san-jit: build/san/skj-jit
	@echo "== check-san: C via the JIT (skj-jit, ASan+UBSan) =="
	@$(SAN_RUNENV) JIT="$(CURDIR)/build/san/skj-jit" sh tests/run-jit-tests.sh

check-san-scheme: build/san/skj-sc
	@echo "== check-san: TinScheme front end (skj-sc, compile-only) =="
	@$(SAN_RUNENV) sh tests/run-compile-san.sh "$(CURDIR)/build/san/skj-sc" scheme $(SCM_TESTS)

check-san-moo: build/san/skj-mooc
	@echo "== check-san: MooScript front end (skj-mooc, compile-only) =="
	@$(SAN_RUNENV) sh tests/run-compile-san.sh "$(CURDIR)/build/san/skj-mooc" moo $(MOO_TESTS) $(MOO_TOY_TESTS)

check-san-pc: build/san/skj-pc
	@echo "== check-san: Pascal front end (skj-pc, compile-only) =="
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
## ColdFire seeds: a spread of C compiler outputs (recursion, loops, arrays,
## branches, nested structs, nested calls) so the mutations reach a broad slice
## of the encoder.
AS_FUZZ_CF_PROGS := cc_t018_recursive cc_t032_nested_loop cc_t010_array \
                    cc_t030_switch_fallthrough cc_t025_nested_struct cc_t014_nested_call
AS_FUZZ_CF_SEEDS := $(AS_FUZZ_CF_PROGS:%=build/as-fuzz-seeds/%.s)
build/as-fuzz-seeds/%.s: tests/%.c build/skj-cc | build
	@mkdir -p build/as-fuzz-seeds
	@./build/skj-cc -o $@ $<
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
## C compiler, every IR front end (scheme, moo, Pascal), the
## assembler and linker, and the archive tool.  This is the CI
## slice: self-contained, no RISC-V reference assembler and no cross checkouts.
check-san-tools: check-san-cpp check-san-cc \
                 check-san-scheme check-san-moo check-san-pc \
                 check-san-ar
	@echo "check-san-tools: preprocessor, compilers, front ends, assembler, linker, and ar clean under the sanitizers."

## The Kobold behavioral corpora through the ASan+UBSan driver.
check-san-kobold: check-san-kp check-san-kp-corpus check-san-kp-multi \
                  check-san-kp-io check-san-kp-inc
	@echo "check-san-kobold: KPM1 reader and the Kobold corpora clean under ASan+UBSan."

## The sanitized slices grouped by target architecture, so CI can run them as
## parallel jobs.  Their union is exactly check-san (every slice, no overlap):
## each per-arch group is that backend's sanitized tools/corpora and its
## assembler fuzzer, and check-san-fuzz holds the arch-neutral reader and front
## end fuzzers.
check-san-cf: check-san-tools test-as-fuzz
	@echo "check-san-cf: the ColdFire toolchain and corpora clean under the sanitizers."
check-san-rv: check-san-rvas test-as-rv-fuzz
	@echo "check-san-rv: the RV32 assembler/linker clean under the sanitizers."
check-san-x86-64: check-san-jit check-san-kobold test-as-x86-64-fuzz
	@echo "check-san-x86-64: the x86-64 C JIT and the Kobold corpora clean under the sanitizers."
check-san-mips: test-as-mips-fuzz
	@echo "check-san-mips: the MIPS assembler clean under the sanitizers."
check-san-arm64: test-as-aarch64-fuzz
	@echo "check-san-arm64: the AArch64 assembler clean under the sanitizers."
check-san-fuzz: test-kpm-fuzz test-cpp-fuzz \
                test-fe-pascal-fuzz test-fe-moo-fuzz test-fe-cc-fuzz
	@echo "check-san-fuzz: the KPM1 reader, preprocessor, and front-end fuzzers clean under the sanitizers."

## Every sanitized slice.
check-san: check-san-cf check-san-rv check-san-x86-64 check-san-mips check-san-arm64 \
           check-san-fuzz
	@echo "All check-san suites passed under the sanitizers."

.PHONY: check-san check-san-cf check-san-rv check-san-x86-64 \
        check-san-mips check-san-arm64 check-san-fuzz \
        check-san-tools check-san-kobold \
        check-san-cpp check-san-cc check-san-rvas check-san-kp \
        check-san-jit check-san-scheme check-san-moo \
        check-san-pc check-san-ar check-san-kp-corpus check-san-kp-multi \
        check-san-kp-io check-san-kp-inc test-kpm-fuzz test-cpp-fuzz \
        test-as-fuzz test-as-rv-fuzz test-as-mips-fuzz test-as-x86-64-fuzz test-as-aarch64-fuzz \
        test-fe-pascal-fuzz test-fe-moo-fuzz \
        test-fe-cc-fuzz

## arm64 per-test rules: tests/<name>.scm -> build/arm64/<name>.s -> .o -> binary
A64_SCM_TESTS := $(SCM_TESTS)

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

check-arm64: build/skj-sc-arm64 $(A64_SCM_TESTS:tests/%.scm=build/arm64/%)
	@sh tests/run-tests.sh "$(QEMU_A64)" build/arm64

## x86-64 per-test rules: tests/<name>.scm -> build/x86_64/<name>.s -> .o -> binary
X64_SCM_TESTS := $(SCM_TESTS)

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

check-x86-64: build/skj-sc-x86-64 $(X64_SCM_TESTS:tests/%.scm=build/x86_64/%)
	@sh tests/run-tests.sh "$(QEMU_X64)" build/x86_64

## TinScheme compiler.  The front end plus IR is shared; only the backend and
## the width defines vary, so the per-arch source lists are keyed by the
## tool-name suffix and one pattern rule builds every cross target.
SC_FE         := scheme/lex.c scheme/parse.c scheme/print.c scheme/main.c scheme/gc.c scheme/lower.c $(IR_SRC)
SC_SRC        := $(SC_FE) $(BE_CF)
sc_src_rv     := $(SC_FE) $(BE_RV)
sc_src_mips   := $(SC_FE) $(BE_MIPS)
sc_src_arm64  := $(SC_FE) $(BE_A64)
sc_src_x86-64 := $(SC_FE) $(BE_X64)
sc_def_arm64  := -DSKJ_LP64=1
sc_def_x86-64 := -DX86_BITS=64 -DSKJ_LP64=1

build/skj-sc: $(SC_SRC) scheme/scheme.h scheme/gc.h ir/ir.h | build
	$(CC) $(CFLAGS) -Ischeme -Iir -o $@ $(SC_SRC)

build/skj-sc-%: $$(sc_src_$$*) scheme/scheme.h scheme/gc.h ir/ir.h | build
	$(CC) $(CFLAGS) $(sc_def_$*) -Ischeme -Iir -o $@ $(sc_src_$*)

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
                       emu/procfs.c emu/procfs.h \
                       emu/hostplat.c emu/hostplat.h emu/elf32.c emu/elf32.h \
                       emu/elf64.c emu/elf64.h fs/p9_server.c fs/p9_server.h | build
	$(CC) $(CFLAGS) -Iemu -Ifs -o $@ tests/test_guest_mmap.c \
	    emu/guest.c emu/procfs.c emu/hostplat.c emu/elf32.c emu/elf64.c fs/p9_server.c -lm

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
RV32_LOAD := emu/elf32.c emu/elf64.c emu/guest.c emu/procfs.c emu/hostplat.c fs/p9_server.c

## gdbclient speaks the GDB remote protocol over a socket, and the POSIX
## headers for that are hidden by -std=c99.
RV32_SOCK_CFLAGS := $(subst -std=c99,-std=gnu99,$(CFLAGS))

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

## Pascal compiler
PC_SRC := pascal/lex.c pascal/parse.c pascal/lower.c pascal/main.c $(IR_SRC) $(BE_CF)

build/skj-pc: $(PC_SRC) pascal/pascal.h ir/ir.h | build
	$(CC) $(CFLAGS) -Ipascal -Iir -o $@ $(PC_SRC)

## ---- Native host Pascal compilers (x86-64, aarch64) ----
## The front end is backend-agnostic; each host target is a separate binary
## linked with that target's shared selector, mirroring skj-cc.  -DSKJ_LP64
## tells the front end the target is LP64, so it emits the 64-bit address
## opcodes (IR_LEA64, IR_LDL64, IR_LD64, ...) the IR reserves for a 64-bit
## pointer.  Keyed by the tool-name suffix and built by one pattern rule.
PC_FE         := pascal/lex.c pascal/parse.c pascal/lower.c pascal/main.c
pc_src_x86-64 := $(PC_FE) $(IR_SRC) $(BE_X64_SEL)
pc_src_arm64  := $(PC_FE) $(IR_SRC) $(BE_A64_SEL)
pc_flags_x86-64 := -DX86_BITS=64 -DSKJ_LP64=1 -Ipascal -Iir -Ibackend
pc_flags_arm64  := -DSKJ_LP64=1 -Ipascal -Iir -Ibackend
## The MIPS Pascal compiler is soft-float (the PlayStation R3051 has no FPU) and
## ILP32, the counterpart of skj-cc-mips-sf. It shares the MIPS backend.
pc_src_mips   := $(PC_FE) $(IR_SRC) $(BE_MIPS)
pc_flags_mips := -DMIPS_SOFTFLOAT -Ipascal -Iir -Ibackend

build/skj-pc-%: $$(pc_src_$$*) pascal/pascal.h ir/ir.h $(wildcard backend/*.h) | build
	$(CC) $(CFLAGS) $(pc_flags_$*) -o $@ $(pc_src_$*)

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

## kpcom acceptance: the Kobold compiler assembled two independent ways must emit
## the same module.  Both are single-step (driving the C builder, serializing with
## kpm_write, no text emitter), but from different pipelines: skj-kpc-rv is kpcom
## compiled ahead-of-time by skj-pc and linked to the builder (a host binary, rv32
## KPM); skj-kp-jit-rv is kpcom JIT'd from its embedded rv32 seed, driving the same
## builder through its bound kpbp_* (a cross binary, run under qemu-riscv32).  Both
## target rv32, so a byte-identical KPM over the corpus proves the native-link path
## and the JIT'd-seed path agree.  rv, not x86-64: the x86-64 skj-kp-jit links the
## same kpcom object as skj-kpc, so that pair would only compare a build to itself.
check-pc-kpcom: build/skj-kpc-rv build/skj-kp-jit-rv
	@KPC=build/skj-kpc-rv REF=build/skj-kp-jit-rv QEMU="$(QEMU_RV)" \
	    CORPUS=tests/kp-corpus OUT=build sh tests/run-pc-kpcom.sh
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
test-kp-rv-loc: build/skj-kpc-rv build/skj-kpc-rv build/skj-as-rv
	@mkdir -p build/debug
	@./build/skj-kpc-rv --emit-kpm -g -o build/debug/kloc_g.kpm tests/debug/loc.pas
	@./build/skj-kpc-rv --emit-kpm    -o build/debug/kloc_n.kpm tests/debug/loc.pas
	@./build/skj-kpc-rv -o build/debug/kloc_g.s build/debug/kloc_g.kpm
	@./build/skj-kpc-rv -o build/debug/kloc_n.s build/debug/kloc_n.kpm
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
## reads.  skj-kpc-rv emits the LOC under -g (the shared rv backend, so
## .loc/.type/.size come for free), skj-as-rv builds the
## .debug_* sections, and skj-ld-rv relocates them and re-emits the .symtab.
## skj-addr2line must then agree with GNU addr2line on function and line for
## every statement, proving a Kobold crash or breakpoint symbolizes like a C one.
test-kp-rv-dwarf: build/skj-kpc-rv build/skj-kpc-rv build/skj-as-rv build/skj-ld-rv \
                  build/skj-addr2line build/kpfree-rv/kp_crt_rv.o build/libkpaot-rv-free.a
	@mkdir -p build/debug
	@./build/skj-kpc-rv --emit-kpm -g -o build/debug/kdw.kpm tests/debug/loc.pas
	@./build/skj-kpc-rv -o build/debug/kdw.s build/debug/kdw.kpm
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
test-kp-rv-trap-backtrace: build/skj-kpc-rv build/skj-kpc-rv build/skj-as-rv build/skj-ld-rv \
                           build/skj-run build/kpfree-rv/kp_crt_rv.o build/libkpaot-rv-free.a
	@mkdir -p build/debug
	@./build/skj-kpc-rv --emit-kpm -g -o build/debug/ktrap.kpm tests/debug/ktrap.pas
	@./build/skj-kpc-rv -o build/debug/ktrap.s build/debug/ktrap.kpm
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

## Debug-info line markers (-g) on the Pascal path: skj-pc emits an
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
## m68k executable whose DWARF a stock toolchain reads.  skj-kpc-cf shares the cf
## backend (so .loc/.type/.size come for free), skj-as builds the big-endian
## .debug_* sections (R_68K_32), and skj-ld relocates them; skj-addr2line must
## then agree with GNU addr2line on function and line for every statement.
test-kp-cf-dwarf: build/skj-kpc-cf build/skj-kpc-cf build/skj-as build/skj-ld \
                  build/skj-addr2line build/kpfree-cf/kp_crt_cf.o build/libkpaot-cf-free.a
	@mkdir -p build/debug
	@./build/skj-kpc-cf --emit-kpm -g -o build/debug/kdwcf.kpm tests/debug/loc.pas
	@./build/skj-kpc-cf -o build/debug/kdwcf.s build/debug/kdwcf.kpm
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
test-kp-cf-trap-backtrace: build/skj-kpc-cf build/skj-kpc-cf build/skj-as build/skj-ld \
                           build/skj-run build/kpfree-cf/kp_crt_cf.o build/libkpaot-cf-free.a
	@mkdir -p build/debug
	@./build/skj-kpc-cf --emit-kpm -g -o build/debug/ktrapcf.kpm tests/debug/ktrap.pas
	@./build/skj-kpc-cf -o build/debug/ktrapcf.s build/debug/ktrapcf.kpm
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

## The MIPS twin of test-rv-backtrace: a nested null-read fault, and skj-run
## walks the $fp (s8) chain and symbolizes it through the shared DWARF reader.
## The frames must name the functions in call order (level3 inner -> main
## outer); the crt _start frame is NOTYPE and prints ??.
test-mips-backtrace: build/skj-cc-mips build/skj-as-mips build/skj-ld-mips build/skj-run build/mips/start.o
	@mkdir -p build/debug
	@./build/skj-cc-mips -g -o build/debug/mcrash.s tests/debug/crash.c
	@./build/skj-as-mips -o build/debug/mcrash.o build/debug/mcrash.s
	@./build/skj-ld-mips -o build/debug/mcrash.elf build/mips/start.o build/debug/mcrash.o
	@./build/skj-run build/debug/mcrash.elf 2>build/debug/mcrash.err; \
	 bt=`grep '^#' build/debug/mcrash.err | sed 's/.*0x[0-9a-f]* *//; s/ at .*//'`; \
	 order=`echo "$$bt" | grep -E 'level3|level2|level1|main' | tr '\n' ' '`; \
	 if [ "$$order" = "level3 level2 level1 main " ]; then \
	     echo "PASS: skj-run MIPS backtrace walks level3 -> level2 -> level1 -> main"; \
	 else \
	     echo "FAIL: backtrace order was [$$order]"; \
	     cat build/debug/mcrash.err; exit 1; \
	 fi

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

## FP registers in the target description and write watchpoints (Z2), driven
## through the RISC-V stub with gdb-multiarch: a 64-bit FP register round-trips
## through p/P, and a write watchpoint on a global stops the guest at the write.
test-rv-gdb-fpwatch: build/skj-cc-rv build/skj-as-rv build/skj-ld-rv build/skj-run build/rv/start.o
	@if ! command -v $(RV_GDB) >/dev/null 2>&1; then \
	     echo "SKIP: test-rv-gdb-fpwatch ($(RV_GDB) not found)"; exit 0; fi; \
	 mkdir -p build/debug; \
	 ./build/skj-cc-rv -g -o build/debug/watch.s tests/debug/watch.c; \
	 ./build/skj-as-rv -o build/debug/watch.o build/debug/watch.s; \
	 ./build/skj-ld-rv -o build/debug/watch.elf build/rv/start.o build/debug/watch.o; \
	 port=$$((20000 + ($$$$ + 2) % 20000)); \
	 ./build/skj-run --gdb $$port build/debug/watch.elf >/dev/null 2>build/debug/w.err & rp=$$!; \
	 for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do \
	     grep -q listening build/debug/w.err 2>/dev/null && break; sleep 0.2; done; \
	 $(RV_GDB) -q -nx -batch -ex "set pagination off" \
	     -ex "target remote 127.0.0.1:$$port" -ex "break main" -ex "continue" \
	     -ex "set \$$ft0 = 2.5" -ex "print \$$ft0" -ex "print \$$fcsr" \
	     -ex "watch *(int *)&gcounter" -ex "continue" build/debug/watch.elf \
	     >build/debug/w.out 2>&1; \
	 kill $$rp 2>/dev/null; wait $$rp 2>/dev/null; \
	 ok=1; \
	 grep -q "2.5" build/debug/w.out || { echo "FAIL: FP register \$$ft0 did not round-trip"; ok=0; }; \
	 grep -qiE "watchpoint" build/debug/w.out || { echo "FAIL: watchpoint not set"; ok=0; }; \
	 grep -q "New value = 1" build/debug/w.out || { echo "FAIL: watchpoint did not fire on write"; ok=0; }; \
	 if [ $$ok = 1 ]; then \
	     echo "PASS: RISC-V stub serves FP registers and a write watchpoint"; \
	 else cat build/debug/w.out; exit 1; fi

## The MIPS twin of test-rv-gdb, driven through the MIPS stub (emu/mips_gdb.c)
## with gdb-multiarch as the oracle: a break at main, the clean exit code, and a
## SIGSEGV whose faulting frame (level3) and its caller (level2) symbolize.  gdb
## stops the MIPS unwind there: skj-cc emits .debug_line but no CFI, so gdb falls
## back to prologue analysis, which truncates this call chain at two frames.
## That is a gdb MIPS-unwinder limit, not a stub one; qemu-mipsel's own gdbstub
## truncates the same binary identically, and skj-run's native DWARF backtrace
## (test-mips-addr2line path) walks all four frames.  Skips if gdb is absent.
MIPS_GDB ?= gdb-multiarch
test-mips-gdb: build/skj-cc-mips build/skj-as-mips build/skj-ld-mips build/skj-run build/mips/start.o
	@if ! command -v $(MIPS_GDB) >/dev/null 2>&1; then \
	     echo "SKIP: test-mips-gdb ($(MIPS_GDB) not found)"; exit 0; fi; \
	 mkdir -p build/debug; \
	 ./build/skj-cc-mips -g -o build/debug/mloc.s tests/debug/loc.c; \
	 ./build/skj-as-mips -o build/debug/mloc.o build/debug/mloc.s; \
	 ./build/skj-ld-mips -o build/debug/mloc.elf build/mips/start.o build/debug/mloc.o; \
	 ./build/skj-cc-mips -g -o build/debug/mcrash.s tests/debug/crash.c; \
	 ./build/skj-as-mips -o build/debug/mcrash.o build/debug/mcrash.s; \
	 ./build/skj-ld-mips -o build/debug/mcrash.elf build/mips/start.o build/debug/mcrash.o; \
	 port=$$((20000 + $$$$ % 20000)); \
	 ./build/skj-run --gdb $$port build/debug/mloc.elf >/dev/null 2>build/debug/mg1.err & rp=$$!; \
	 for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do \
	     grep -q listening build/debug/mg1.err 2>/dev/null && break; sleep 0.2; done; \
	 $(MIPS_GDB) -q -nx -batch -ex "set pagination off" \
	     -ex "target remote 127.0.0.1:$$port" -ex "break main" \
	     -ex "continue" -ex "continue" build/debug/mloc.elf \
	     >build/debug/mg1.out 2>&1; \
	 wait $$rp 2>/dev/null; \
	 port=$$((20000 + ($$$$ + 1) % 20000)); \
	 ./build/skj-run --gdb $$port build/debug/mcrash.elf >/dev/null 2>build/debug/mg2.err & rp=$$!; \
	 for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do \
	     grep -q listening build/debug/mg2.err 2>/dev/null && break; sleep 0.2; done; \
	 $(MIPS_GDB) -q -nx -batch -ex "set pagination off" \
	     -ex "target remote 127.0.0.1:$$port" -ex "continue" \
	     -ex "backtrace" build/debug/mcrash.elf >build/debug/mg2.out 2>&1; \
	 kill $$rp 2>/dev/null; wait $$rp 2>/dev/null; \
	 ok=1; \
	 grep -q "Breakpoint 1, main" build/debug/mg1.out || { echo "FAIL: no break at main"; ok=0; }; \
	 grep -q "exited with code 05" build/debug/mg1.out || { echo "FAIL: loc did not exit 05"; ok=0; }; \
	 grep -q "SIGSEGV" build/debug/mg2.out || { echo "FAIL: no SIGSEGV on crash"; ok=0; }; \
	 for fn in level3 level2; do \
	     grep -q "in $$fn " build/debug/mg2.out || { echo "FAIL: backtrace missing $$fn"; ok=0; }; done; \
	 if [ $$ok = 1 ]; then \
	     echo "PASS: gdb-multiarch drives the skj-run MIPS RSP stub (break, fault frame, exit)"; \
	 else cat build/debug/mg1.out build/debug/mg2.out; exit 1; fi

## The MIPS twin of test-rv-gdb-fpwatch: an FP register round-trips through the
## stub's p/P (gdb's o32 f0-f31 at regnums 38-69) and a write watchpoint fires.
test-mips-gdb-fpwatch: build/skj-cc-mips build/skj-as-mips build/skj-ld-mips build/skj-run build/mips/start.o
	@if ! command -v $(MIPS_GDB) >/dev/null 2>&1; then \
	     echo "SKIP: test-mips-gdb-fpwatch ($(MIPS_GDB) not found)"; exit 0; fi; \
	 mkdir -p build/debug; \
	 ./build/skj-cc-mips -g -o build/debug/mwatch.s tests/debug/watch.c; \
	 ./build/skj-as-mips -o build/debug/mwatch.o build/debug/mwatch.s; \
	 ./build/skj-ld-mips -o build/debug/mwatch.elf build/mips/start.o build/debug/mwatch.o; \
	 port=$$((20000 + ($$$$ + 3) % 20000)); \
	 ./build/skj-run --gdb $$port build/debug/mwatch.elf >/dev/null 2>build/debug/mw.err & rp=$$!; \
	 for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do \
	     grep -q listening build/debug/mw.err 2>/dev/null && break; sleep 0.2; done; \
	 $(MIPS_GDB) -q -nx -batch -ex "set pagination off" \
	     -ex "target remote 127.0.0.1:$$port" -ex "break main" -ex "continue" \
	     -ex "set \$$f0 = 2.5" -ex "print \$$f0" \
	     -ex "watch *(int *)&gcounter" -ex "continue" build/debug/mwatch.elf \
	     >build/debug/mw.out 2>&1; \
	 kill $$rp 2>/dev/null; wait $$rp 2>/dev/null; \
	 ok=1; \
	 grep -q "2.5" build/debug/mw.out || { echo "FAIL: FP register \$$f0 did not round-trip"; ok=0; }; \
	 grep -qiE "watchpoint" build/debug/mw.out || { echo "FAIL: watchpoint not set"; ok=0; }; \
	 grep -q "New value = 1" build/debug/mw.out || { echo "FAIL: watchpoint did not fire on write"; ok=0; }; \
	 if [ $$ok = 1 ]; then \
	     echo "PASS: MIPS stub serves FP registers and a write watchpoint"; \
	 else cat build/debug/mw.out; exit 1; fi

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

## Multi-sandbox debugging: two debugger+sandbox pairs at once.  A sandbox is one
## gdb process; debugging several means running several skj-run --gdb instances,
## each on its own port with its own debugger attached.  The stub binds the port
## it is handed and keeps all its state per connection (no process-global state),
## so concurrent instances do not interfere.  This runs two skj-dbg sessions in
## parallel on distinct ports -- a breakpoint session and a crash-backtrace
## session -- and checks each still gets its own correct, symbolized result.
test-rv-dbg-multi: build/skj-cc-rv build/skj-as-rv build/skj-ld-rv build/skj-run build/skj-dbg build/rv/start.o
	@mkdir -p build/debug
	@./build/skj-cc-rv -g -o build/debug/loc.s tests/debug/loc.c
	@./build/skj-as-rv -o build/debug/loc.o build/debug/loc.s
	@./build/skj-ld-rv -o build/debug/loc.elf build/rv/start.o build/debug/loc.o
	@./build/skj-cc-rv -g -o build/debug/crash.s tests/debug/crash.c
	@./build/skj-as-rv -o build/debug/crash.o build/debug/crash.s
	@./build/skj-ld-rv -o build/debug/crash.elf build/rv/start.o build/debug/crash.o
	@./build/skj-dbg --port 21877 --run ./build/skj-run -ex "break main" -ex "continue" \
	     build/debug/loc.elf >build/debug/multi1.out 2>&1 & p1=$$!; \
	 ./build/skj-dbg --port 21879 --run ./build/skj-run -ex "continue" -ex "backtrace" \
	     build/debug/crash.elf >build/debug/multi2.out 2>&1 & p2=$$!; \
	 wait $$p1; wait $$p2; \
	 ok=1; \
	 grep -q "breakpoint at .*main at .*loc.c:" build/debug/multi1.out || { echo "FAIL: instance 1 break not symbolized"; ok=0; }; \
	 grep -q "stopped in main at .*loc.c:" build/debug/multi1.out || { echo "FAIL: instance 1 did not stop in main"; ok=0; }; \
	 grep -q "SIGSEGV.*in level3 at .*crash.c:" build/debug/multi2.out || { echo "FAIL: instance 2 no symbolized SIGSEGV"; ok=0; }; \
	 bt=`grep '^#' build/debug/multi2.out | sed 's/.*0x[0-9a-f]* *//; s/ at .*//' | grep -E 'level|main' | tr '\n' ' '`; \
	 if [ "$$bt" != "level3 level2 level1 main " ]; then echo "FAIL: instance 2 bt order [$$bt]"; ok=0; fi; \
	 if [ $$ok = 1 ]; then \
	     echo "PASS: two concurrent skj-dbg sessions on distinct sandbox instances"; \
	 else cat build/debug/multi1.out build/debug/multi2.out; exit 1; fi

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
## The 64-bit hosted targets have no libc archive (dev/test tier); the driver
## links the crt and runtime helpers only.
DRIVER_LIB_X64  := build/lib/x86-64/crt0.o build/lib/x86-64/half.o build/lib/x86-64/va.o
DRIVER_LIB_A64  := build/lib/arm64/crt0.o build/lib/arm64/va.o

## The coop_echo guest for test-coop-read, built through the skjcc driver for
## each sandbox core.  Placed here (not beside test-coop-read) so DRIVER_LIB_*
## is already defined when these prerequisite lists are read.
build/coop_echo: tests/coop_echo.c build/skjcc build/skj-cc build/skj-as build/skj-ld $(DRIVER_LIB_CF) | build
	@./build/skjcc -target coldfire -o $@ $<
build/rv/coop_echo: tests/coop_echo.c build/skjcc build/skj-cc-rv build/skj-as-rv build/skj-ld-rv $(DRIVER_LIB_RV) | build/rv
	@./build/skjcc -target riscv -o $@ $<
build/mips/coop_echo: tests/coop_echo.c build/skjcc build/skj-cc-mips build/skj-as-mips build/skj-ld-mips $(DRIVER_LIB_MIPS) | build/mips
	@./build/skjcc -target mips -o $@ $<

## ---- install / uninstall: stage the built toolchain under a PREFIX ----
## Installs the host tools, the C headers, the per-target C runtime the skjcc
## driver links (crt0, the helper objects, libc.a), and the Kobold AOT runtime
## archives.  PREFIX defaults to the user's ~/.local; DESTDIR prepends a staging
## root for a packaged install.  The layout mirrors what the tools look up at
## run time relative to their own location: headers under <libdir>/include and a
## per-target runtime dir <libdir>/<target>, found as <bindir>/../lib/skjegg
## beside the installed binary.  So an installed copy needs only PATH; the tools
## keep the in-tree build paths as their baked fallback and SKJ_CC_INCLUDE /
## SKJ_CC_LIB as explicit overrides.  This placement is deliberate: a prerequisite list is
## expanded as the rule is read, so install must sit below the DRIVER_LIB_*
## definitions above or those prerequisites expand empty.
PREFIX  ?= $(HOME)/.local
bindir  := $(PREFIX)/bin
libdir  := $(PREFIX)/lib/skjegg
incdir  := $(libdir)/include
INSTALL ?= install

INSTALL_KPAOT := build/libkpaot.a build/libkpaot-rv.a build/libkpaot-arm64.a \
                 build/libkpaot-cf.a build/libkpaot-rv-free.a build/libkpaot-cf-free.a

# The PlayStation (MIPS PS-EXE) toolchain and the libpsx SDK, so a project
# outside the tree can build PS1 programs. The skjcc `ps1` target drives the C
# path; skj-pc-mips and skj-kpc-mips build Pascal and Kobold.
# See doc/tools/psx-setup.md.
INSTALL_PSX_TOOLS := build/skj-cc-mips-sf build/skj-pc-mips build/skj-kpc-mips \
                     build/skj-kpc-mips build/skj-mkromfs \
                     build/skj-timconv build/skj-vagconv build/skj-mkcdda
INSTALL_PSX_SDK := build/mips/libpsx.a build/mips/start_psx.o \
                   build/psx-units/psx.kpm build/psx/psx_unit.o

install: $(TOOLS) \
         $(DRIVER_LIB_CF) $(DRIVER_LIB_RV) $(DRIVER_LIB_MIPS) \
         $(DRIVER_LIB_X64) $(DRIVER_LIB_A64) $(INSTALL_KPAOT) \
         $(INSTALL_PSX_TOOLS) $(INSTALL_PSX_SDK)
	@echo "  INSTALL  tools -> $(DESTDIR)$(bindir)"
	@$(INSTALL) -d $(DESTDIR)$(bindir)
	@$(INSTALL) -m 755 $(TOOLS) $(DESTDIR)$(bindir)/
	@echo "  INSTALL  headers -> $(DESTDIR)$(incdir)"
	@$(INSTALL) -d $(DESTDIR)$(incdir)/sys
	@$(INSTALL) -m 644 include/*.h $(DESTDIR)$(incdir)/
	@$(INSTALL) -m 644 include/sys/*.h $(DESTDIR)$(incdir)/sys/
	@echo "  INSTALL  C runtime -> $(DESTDIR)$(libdir)/<target>"
	@$(INSTALL) -d $(DESTDIR)$(libdir)/coldfire && $(INSTALL) -m 644 $(DRIVER_LIB_CF)   $(DESTDIR)$(libdir)/coldfire/
	@$(INSTALL) -d $(DESTDIR)$(libdir)/riscv    && $(INSTALL) -m 644 $(DRIVER_LIB_RV)   $(DESTDIR)$(libdir)/riscv/
	@$(INSTALL) -d $(DESTDIR)$(libdir)/mips     && $(INSTALL) -m 644 $(DRIVER_LIB_MIPS) $(DESTDIR)$(libdir)/mips/
	@$(INSTALL) -d $(DESTDIR)$(libdir)/x86-64   && $(INSTALL) -m 644 $(DRIVER_LIB_X64)  $(DESTDIR)$(libdir)/x86-64/
	@$(INSTALL) -d $(DESTDIR)$(libdir)/arm64    && $(INSTALL) -m 644 $(DRIVER_LIB_A64)  $(DESTDIR)$(libdir)/arm64/
	@echo "  INSTALL  Kobold runtime -> $(DESTDIR)$(libdir)/kobold"
	@$(INSTALL) -d $(DESTDIR)$(libdir)/kobold && $(INSTALL) -m 644 $(INSTALL_KPAOT) $(DESTDIR)$(libdir)/kobold/
	@echo "  INSTALL  PlayStation toolchain -> $(DESTDIR)$(bindir)"
	@$(INSTALL) -m 755 $(INSTALL_PSX_TOOLS) $(DESTDIR)$(bindir)/
	@echo "  INSTALL  libpsx SDK -> $(DESTDIR)$(libdir)/ps1"
	@$(INSTALL) -d $(DESTDIR)$(libdir)/ps1
	@$(INSTALL) -m 644 build/mips/libpsx.a build/mips/start_psx.o $(DESTDIR)$(libdir)/ps1/
	@$(INSTALL) -m 644 libpsx/psx.h units/psx.pas units/psx.kp $(DESTDIR)$(libdir)/ps1/
	@$(INSTALL) -m 644 build/psx-units/psx.kpm build/psx/psx_unit.o $(DESTDIR)$(libdir)/ps1/
	@echo
	@echo "Installed skjegg $(SKJ_VERSION) under $(DESTDIR)$(PREFIX)."
	@echo "The tools find their headers and runtime relative to their own"
	@echo "location, so only PATH is needed:"
	@echo "    export PATH=\"$(bindir):\$$PATH\""
	@echo "(SKJ_CC_INCLUDE / SKJ_CC_LIB still override, if ever needed.)"

uninstall:
	@echo "  UNINSTALL  $(DESTDIR)$(bindir) tools + $(DESTDIR)$(libdir)"
	@for t in $(notdir $(TOOLS) $(INSTALL_PSX_TOOLS)); do rm -f "$(DESTDIR)$(bindir)/$$t"; done
	@rm -rf "$(DESTDIR)$(libdir)"

## The skjcc driver end to end on the 64-bit targets, with no external
## toolchain: the driver runs skj-cc-x86-64/skj-cc-arm64, skj-as-x86-64/
## skj-as-aarch64 and the in-tree ELF64 linkers skj-ld-x86-64/skj-ld-arm64,
## then the crt + runtime helpers.  The program returns 45; x86-64 runs
## natively or under qemu-x86_64, arm64 under qemu-aarch64.
test-cc-driver-x86-64: build/skjcc $(DRIVER_LIB_X64) | build
	@./build/skjcc -target x86-64 -o build/drv_x86_64 tests/ld/driver_ret.c
	@$(QEMU_X64) ./build/drv_x86_64; ec=$$?; \
	 if [ "$$ec" = 45 ]; then \
	     echo "PASS: skjcc -target x86-64 (cc -> skj-as-x86-64 -> skj-ld-x86-64, exit 45)"; \
	 else echo "FAIL: skjcc -target x86-64 driver (exit $$ec)"; exit 1; fi

test-cc-driver-arm64: build/skjcc $(DRIVER_LIB_A64) | build
	@./build/skjcc -target arm64 -o build/drv_arm64 tests/ld/driver_ret.c
	@$(QEMU_A64) ./build/drv_arm64; ec=$$?; \
	 if [ "$$ec" = 45 ]; then \
	     echo "PASS: skjcc -target arm64 (cc -> skj-as-aarch64 -> skj-ld-arm64, exit 45)"; \
	 else echo "FAIL: skjcc -target arm64 driver (exit $$ec)"; exit 1; fi

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

# Per-arch toolchain + runtime-object environment for the freestanding libc
# test suites (CCBIN/ASM/LD/START/HALF).  Both the check-libc-<arch> suites and
# the fs-interpose fs-cap/dev/rom/9p tests use the same per-arch values; each
# call site adds its own QEMU, OUTDIR, TEST and FS_* toggles.  The ColdFire
# fragment sets only HALF and leans on run-libc-tests.sh's defaults (skj-cc,
# skj-as, m68k-linux-gnu-ld, build/start.o), as the recipes did before.
LIBC_ENV_cf = HALF="$(CURDIR)/build/half.o $(CURDIR)/build/va_cf.o $(CURDIR)/build/co_switch_cf.o"
LIBC_ENV_rv = CCBIN="$(CURDIR)/build/skj-cc-rv" ASM="$(RV_AS) -march=rv32imfd_zfh -mabi=ilp32 -o" LD="$(RV_LD) -m elf32lriscv -o" START="$(CURDIR)/build/rv/start.o" HALF="$(CURDIR)/build/rv/va_rv.o $(CURDIR)/build/rv/soft64_div.o $(CURDIR)/build/rv/co_switch.o"
LIBC_ENV_mips = CCBIN="$(CURDIR)/build/skj-cc-mips" ASM="$(MIPS_AS) -march=mips1 -EL -o" LD="$(MIPS_LD) -o" START="$(CURDIR)/build/mips/start.o" HALF="$(CURDIR)/build/mips/half.o $(CURDIR)/build/mips/va_mips.o $(CURDIR)/build/mips/soft64_div.o $(CURDIR)/build/mips/co_switch.o"
LIBC_ENV_x86_64 = CCBIN="$(CURDIR)/build/skj-cc-x86-64" ASM="$(X64_ASM) $(X64_ASMFLAGS) -o" LD="$(X64_LD) -m elf_x86_64 -o" START="$(CURDIR)/build/x86_64/start_sysv.o" HALF="$(CURDIR)/build/x86_64/half.o $(CURDIR)/build/x86_64/va.o $(CURDIR)/build/x86_64/soft64.o $(CURDIR)/build/x86_64/co_switch.o"
LIBC_ENV_arm64 = CCBIN="$(CURDIR)/build/skj-cc-arm64" ASM="$(A64_AS) -o" LD="$(A64_LD) -o" START="$(CURDIR)/build/arm64/start_aapcs.o" HALF="$(CURDIR)/build/arm64/va.o $(CURDIR)/build/arm64/soft64.o $(CURDIR)/build/arm64/co_switch.o"

test-rv-fs-cap: build/skj-cc-rv build/rv/start.o build/rv/va_rv.o build/rv/soft64_div.o build/rv/co_switch.o $(FS_CAP_RUN)
	@rm -rf build/fs-cap-rv
	@mkdir -p build/fs-cap-rv/data
	@printf 'capability-granted\n' > build/fs-cap-rv/data/seed.txt
	@printf 'ESCAPED\n' > build/fs-cap-rv/escape.txt
	@$(LIBC_ENV_rv) \
	 QEMU="$(CURDIR)/$(FS_CAP_RUN) --mount data:$(CURDIR)/build/fs-cap-rv/data" \
	 OUTDIR="$(CURDIR)/build/fs-cap-rv" TEST="$(CURDIR)/tests/fs-cap/capguest.c" \
	 FS_INTERPOSE=1 EXPECT=42 \
	 sh tests/run-libc-tests.sh

test-cf-fs-cap: build/skj-cc build/skj-as build/start.o build/half.o build/va_cf.o build/co_switch_cf.o $(FS_CAP_RUN)
	@rm -rf build/fs-cap-cf
	@mkdir -p build/fs-cap-cf/data
	@printf 'capability-granted\n' > build/fs-cap-cf/data/seed.txt
	@printf 'ESCAPED\n' > build/fs-cap-cf/escape.txt
	@$(LIBC_ENV_cf) \
	 QEMU="$(CURDIR)/$(FS_CAP_RUN) --mount data:$(CURDIR)/build/fs-cap-cf/data" \
	 OUTDIR="$(CURDIR)/build/fs-cap-cf" TEST="$(CURDIR)/tests/fs-cap/capguest.c" \
	 FS_INTERPOSE=1 EXPECT=42 \
	 sh tests/run-libc-tests.sh

test-mips-fs-cap: build/skj-cc-mips build/mips/start.o build/mips/half.o build/mips/va_mips.o build/mips/soft64_div.o build/mips/co_switch.o $(FS_CAP_RUN)
	@rm -rf build/fs-cap-mips
	@mkdir -p build/fs-cap-mips/data
	@printf 'capability-granted\n' > build/fs-cap-mips/data/seed.txt
	@printf 'ESCAPED\n' > build/fs-cap-mips/escape.txt
	@$(LIBC_ENV_mips) \
	 QEMU="$(CURDIR)/$(FS_CAP_RUN) --mount data:$(CURDIR)/build/fs-cap-mips/data" \
	 OUTDIR="$(CURDIR)/build/fs-cap-mips" TEST="$(CURDIR)/tests/fs-cap/capguest.c" \
	 FS_INTERPOSE=1 EXPECT=42 \
	 sh tests/run-libc-tests.sh

## devfs over the launcher --dev grant: a guest reaches the in-guest device
## backend (null/zero/full) through a "dev:" mount the launcher installs, with
## no host resource.  Same freestanding fs/-interpose libc and skj-run driver as
## the fs-cap test; the guest self-checks and exits 42.
test-rv-fs-dev: build/skj-cc-rv build/rv/start.o build/rv/va_rv.o build/rv/soft64_div.o build/rv/co_switch.o $(FS_CAP_RUN)
	@rm -rf build/fs-dev-rv
	@mkdir -p build/fs-dev-rv
	@$(LIBC_ENV_rv) \
	 QEMU="$(CURDIR)/$(FS_CAP_RUN) --dev dev" \
	 OUTDIR="$(CURDIR)/build/fs-dev-rv" TEST="$(CURDIR)/tests/fs-dev/devguest.c" \
	 FS_INTERPOSE=1 FS_DEV=1 EXPECT=42 \
	 sh tests/run-libc-tests.sh

test-cf-fs-dev: build/skj-cc build/skj-as build/start.o build/half.o build/va_cf.o build/co_switch_cf.o $(FS_CAP_RUN)
	@rm -rf build/fs-dev-cf
	@mkdir -p build/fs-dev-cf
	@$(LIBC_ENV_cf) \
	 QEMU="$(CURDIR)/$(FS_CAP_RUN) --dev dev" \
	 OUTDIR="$(CURDIR)/build/fs-dev-cf" TEST="$(CURDIR)/tests/fs-dev/devguest.c" \
	 FS_INTERPOSE=1 FS_DEV=1 EXPECT=42 \
	 sh tests/run-libc-tests.sh

## romfs over the launcher --rom grant: skj-mkromfs packs a seed directory into
## an image, the launcher loads it into guest memory and records its address in
## fs_mount_aux, and the guest reads it back read-only through a "rom:" mount.
test-rv-fs-rom: build/skj-cc-rv build/rv/start.o build/rv/va_rv.o build/rv/soft64_div.o build/rv/co_switch.o build/skj-mkromfs $(FS_CAP_RUN)
	@rm -rf build/fs-rom-rv
	@mkdir -p build/fs-rom-rv/seed/sub
	@printf 'hello romfs' > build/fs-rom-rv/seed/greet
	@printf 'nested' > build/fs-rom-rv/seed/sub/inner
	@./build/skj-mkromfs build/fs-rom-rv/seed build/fs-rom-rv/rom.img
	@$(LIBC_ENV_rv) \
	 QEMU="$(CURDIR)/$(FS_CAP_RUN) --rom rom:$(CURDIR)/build/fs-rom-rv/rom.img" \
	 OUTDIR="$(CURDIR)/build/fs-rom-rv" TEST="$(CURDIR)/tests/fs-rom/romguest.c" \
	 FS_INTERPOSE=1 FS_ROM=1 EXPECT=42 \
	 sh tests/run-libc-tests.sh

test-cf-fs-rom: build/skj-cc build/skj-as build/start.o build/half.o build/va_cf.o build/co_switch_cf.o build/skj-mkromfs $(FS_CAP_RUN)
	@rm -rf build/fs-rom-cf
	@mkdir -p build/fs-rom-cf/seed/sub
	@printf 'hello romfs' > build/fs-rom-cf/seed/greet
	@printf 'nested' > build/fs-rom-cf/seed/sub/inner
	@./build/skj-mkromfs build/fs-rom-cf/seed build/fs-rom-cf/rom.img
	@$(LIBC_ENV_cf) \
	 QEMU="$(CURDIR)/$(FS_CAP_RUN) --rom rom:$(CURDIR)/build/fs-rom-cf/rom.img" \
	 OUTDIR="$(CURDIR)/build/fs-rom-cf" TEST="$(CURDIR)/tests/fs-rom/romguest.c" \
	 FS_INTERPOSE=1 FS_ROM=1 EXPECT=42 \
	 sh tests/run-libc-tests.sh

## 9pfs over the launcher --9p grant: skj-run serves a host directory with its
## in-process 9P2000 server behind a shared doorbell channel, and the guest 9p
## client reaches it through a "data:" mount.  This is the one place the launcher
## 9p server, the doorbell hostcall and the guest client meet end to end.
test-rv-fs-9p: build/skj-cc-rv build/rv/start.o build/rv/va_rv.o build/rv/soft64_div.o build/rv/co_switch.o $(FS_CAP_RUN)
	@rm -rf build/fs-9p-rv
	@mkdir -p build/fs-9p-rv/data
	@printf '9p-seed\n' > build/fs-9p-rv/data/seed.txt
	@$(LIBC_ENV_rv) \
	 QEMU="$(CURDIR)/$(FS_CAP_RUN) --9p data:$(CURDIR)/build/fs-9p-rv/data" \
	 OUTDIR="$(CURDIR)/build/fs-9p-rv" TEST="$(CURDIR)/tests/fs-9p/p9guest.c" \
	 FS_INTERPOSE=1 FS_9P=1 EXPECT=42 \
	 sh tests/run-libc-tests.sh

test-cf-fs-9p: build/skj-cc build/skj-as build/start.o build/half.o build/va_cf.o build/co_switch_cf.o $(FS_CAP_RUN)
	@rm -rf build/fs-9p-cf
	@mkdir -p build/fs-9p-cf/data
	@printf '9p-seed\n' > build/fs-9p-cf/data/seed.txt
	@$(LIBC_ENV_cf) \
	 QEMU="$(CURDIR)/$(FS_CAP_RUN) --9p data:$(CURDIR)/build/fs-9p-cf/data" \
	 OUTDIR="$(CURDIR)/build/fs-9p-cf" TEST="$(CURDIR)/tests/fs-9p/p9guest.c" \
	 FS_INTERPOSE=1 FS_9P=1 EXPECT=42 \
	 sh tests/run-libc-tests.sh

## Run the fs-cap confinement end to end against build/skj-run-posix, so the
## cap_walk component walk (the portable confinement floor every non-Linux host
## uses) is compiled and exercised on Linux too, where openat2 is the default.
## A recursive make re-drives the fs-cap targets with the forced-POSIX runner.
check-emu-posix: build/skj-run-posix
	@$(MAKE) --no-print-directory test-rv-fs-cap test-cf-fs-cap test-mips-fs-cap FS_CAP_RUN=build/skj-run-posix
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
## entirely by the skj toolchain (skj-kpc -> skj-as -> skj-ld + the freestanding
## runtime), run under skj-run to compile Kobold.  No file-I/O code is needed:
## kobold/io_free.c already supplies ParamStr/ParamCount, ReadFileStr/
## WriteFileStr and the kpd_* directory surface over open/read/write/lseek.

## The freestanding self-host compiler, built single-step with no seed: the host
## skj-kpc-rv compiles kpcom's own source straight to RISC-V assembly, which links
## against the freestanding runtime (the module builder and KPM serializer are in
## libkpaot-rv-free.a, so the in-guest compiler --emit-kpm runs through kpm_write).
build/selfhost/skj-kpc-rv.elf: kobold/kpcom.pas build/skj-kpc-rv build/skj-as-rv \
                               build/skj-ld-rv build/kpfree-rv/kp_crt_rv.o \
                               build/libkpaot-rv-free.a | build
	@mkdir -p build/selfhost
	./build/skj-kpc-rv -D KP_ONESTEP -D KP_TARGET_RV kobold/kpcom.pas -o build/selfhost/kpcom-free-rv.s
	./build/skj-as-rv -o build/selfhost/kpcom-free-rv.o build/selfhost/kpcom-free-rv.s
	./build/skj-ld-rv -o $@ build/kpfree-rv/kp_crt_rv.o \
	    build/selfhost/kpcom-free-rv.o build/libkpaot-rv-free.a

## The ColdFire freestanding self-host compiler, built single-step with no seed
## (the sibling of the rv rule above): the host skj-kpc-cf compiles kpcom's own
## source straight to ColdFire assembly, linked against the freestanding runtime
## whose archive carries the module builder and KPM serializer.
build/selfhost-cf/skj-kpc-cf.elf: kobold/kpcom.pas build/skj-kpc-cf build/skj-as \
                                  build/skj-ld build/kpfree-cf/kp_crt_cf.o \
                                  build/libkpaot-cf-free.a | build
	@mkdir -p build/selfhost-cf
	./build/skj-kpc-cf -D KP_ONESTEP -D KP_TARGET_CF kobold/kpcom.pas -o build/selfhost-cf/kpcom-free-cf.s
	./build/skj-as -o build/selfhost-cf/kpcom-free-cf.o build/selfhost-cf/kpcom-free-cf.s
	./build/skj-ld -o $@ build/kpfree-cf/kp_crt_cf.o \
	    build/selfhost-cf/kpcom-free-cf.o build/libkpaot-cf-free.a

## test-{rv,cf}-selfhost-kpc: run the freestanding skj-kpc guest ELF under
## skj-run to --emit-kpm a small unit-free program (hello.pas) and assert the
## in-guest compiler emits byte-for-byte the same module the host does.  The
## freestanding guest is now the single-step compiler (kpcom compiled with
## KP_ONESTEP straight to native code, no seed), lowering into the C module
## builder and serializing through the same kpm_write the host uses, so the two
## KPM1 images are identical bytes.  This checks the Kobold compiler runs in-guest
## over the guest file I/O (ParamStr, the source read, the KPM write).  The far
## heavier self-compile of kpcom's own 480 KB source is the opt-in -full target
## below (minutes under the RV32/ColdFire interpreter), kept out of the suite.
test-rv-selfhost-kpc: build/selfhost/skj-kpc-rv.elf build/skj-kpc-rv build/skj-run \
                      tests/kp-corpus/hello.pas
	@mkdir -p build/selfhost
	@./build/skj-kpc-rv --emit-kpm tests/kp-corpus/hello.pas > build/selfhost/hello.host.kpm
	@./build/skj-run build/selfhost/skj-kpc-rv.elf --emit-kpm tests/kp-corpus/hello.pas \
	    > build/selfhost/hello.guest.kpm 2>build/selfhost/kpc.err \
	    || { echo "FAIL: in-guest skj-kpc (hello)"; cat build/selfhost/kpc.err; exit 1; }
	@cmp -s build/selfhost/hello.host.kpm build/selfhost/hello.guest.kpm \
	    || { echo "FAIL: in-guest module differs from host (hello)"; \
	         diff build/selfhost/hello.host.kpm build/selfhost/hello.guest.kpm | head -20; exit 1; }
	@echo "PASS: skj-kpc self-hosts under skj-run (RV32): hello emits byte-identical KPM"

test-cf-selfhost-kpc: build/selfhost-cf/skj-kpc-cf.elf build/skj-kpc-cf build/skj-run \
                      tests/kp-corpus/hello.pas
	@mkdir -p build/selfhost-cf
	@./build/skj-kpc-cf --emit-kpm tests/kp-corpus/hello.pas > build/selfhost-cf/hello.host.kpm
	@./build/skj-run build/selfhost-cf/skj-kpc-cf.elf --emit-kpm tests/kp-corpus/hello.pas \
	    > build/selfhost-cf/hello.guest.kpm 2>build/selfhost-cf/kpc.err \
	    || { echo "FAIL: in-guest skj-kpc (hello)"; cat build/selfhost-cf/kpc.err; exit 1; }
	@cmp -s build/selfhost-cf/hello.host.kpm build/selfhost-cf/hello.guest.kpm \
	    || { echo "FAIL: in-guest module differs from host (hello)"; \
	         diff build/selfhost-cf/hello.host.kpm build/selfhost-cf/hello.guest.kpm | head -20; exit 1; }
	@echo "PASS: skj-kpc self-hosts under skj-run (ColdFire): hello emits byte-identical KPM"

## The full self-compile: the in-guest skj-kpc compiles kpcom's own source and
## the emitted module must be byte-for-byte the host's (both serialize the same
## C-builder module through kpm_write now).  Opt-in and slow (the 480 KB source
## through the RV32/ColdFire interpreter takes tens of minutes), so it lives in
## check-slow, not check-all; the light hello.pas self-host (in check-all) proves
## the single-step guest mechanism cheaply.
##
## The single-step guest holds the whole program's IR in memory (the old
## streaming text front end did not).  kp_dynarr grows its storage geometrically
## (dynarr.c), so the one-at-a-time append pattern reallocs O(log n) times and the
## guest's heap tracks its ~50 MB live set rather than the O(N^2) that a
## realloc-per-append would commit on the non-coalescing freestanding gheap.  The
## budget below is generous headroom over that, not a workaround.
KPC_SELFHOST_BUDGET := --heap 256M --mem 512M
test-rv-selfhost-kpc-full: build/selfhost/skj-kpc-rv.elf build/skj-kpc-rv build/skj-run \
                           kobold/kpcom.pas
	@mkdir -p build/selfhost
	@./build/skj-kpc-rv --emit-kpm kobold/kpcom.pas > build/selfhost/kpcom.host.kpm
	@./build/skj-run $(KPC_SELFHOST_BUDGET) build/selfhost/skj-kpc-rv.elf --emit-kpm kobold/kpcom.pas \
	    > build/selfhost/kpcom.guest.kpm 2>build/selfhost/kpc2.err \
	    || { echo "FAIL: in-guest skj-kpc (kpcom self-compile)"; cat build/selfhost/kpc2.err; exit 1; }
	@cmp -s build/selfhost/kpcom.host.kpm build/selfhost/kpcom.guest.kpm \
	    || { echo "FAIL: in-guest kpcom module differs from host"; exit 1; }
	@echo "PASS: skj-kpc compiles its own source in-guest under skj-run (RV32): byte-identical KPM"

test-cf-selfhost-kpc-full: build/selfhost-cf/skj-kpc-cf.elf build/skj-kpc-cf build/skj-run \
                           kobold/kpcom.pas
	@mkdir -p build/selfhost-cf
	@./build/skj-kpc-cf --emit-kpm kobold/kpcom.pas > build/selfhost-cf/kpcom.host.kpm
	@./build/skj-run $(KPC_SELFHOST_BUDGET) build/selfhost-cf/skj-kpc-cf.elf --emit-kpm kobold/kpcom.pas \
	    > build/selfhost-cf/kpcom.guest.kpm 2>build/selfhost-cf/kpc2.err \
	    || { echo "FAIL: in-guest skj-kpc (kpcom self-compile)"; cat build/selfhost-cf/kpc2.err; exit 1; }
	@cmp -s build/selfhost-cf/kpcom.host.kpm build/selfhost-cf/kpcom.guest.kpm \
	    || { echo "FAIL: in-guest kpcom module differs from host"; exit 1; }
	@echo "PASS: skj-kpc compiles its own source in-guest under skj-run (ColdFire): byte-identical KPM"

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
## The MIPS counterpart of test-rv-selfhost: skj-cc built as a MIPS guest (its
## own sources via skjcc -target mips), run under skj-run to compile add.c
## through the sandbox syscalls.  skj-run's MIPS core models the COP1 FPU the
## hardware-float backend targets, so the compiler (which spills callee-saved
## $f20/$f21) runs under the sandbox, not only qemu.  The emitted asm must match
## a qemu run of the same in-guest compiler byte-for-byte; the final program is
## assembled, linked and run under skj-run too.
test-mips-selfhost: build/skjcc build/skj-cc-mips build/skj-as-mips build/skj-ld-mips \
                    $(DRIVER_LIB_MIPS) build/skj-run tests/selfhost/add.c
	@mkdir -p build/selfhost-mips
	@set -e; objs=""; for s in $(CC_SRC_MIPS); do \
	    b=$$(basename $$s .c); \
	    ./build/skjcc -target mips -DCC_MIPS -c -Ibuild -Icc -Icpp -Iir -Ibackend \
	        $$s -o build/selfhost-mips/$$b.o; \
	    objs="$$objs build/selfhost-mips/$$b.o"; \
	 done; \
	 ./build/skjcc -target mips $$objs -o build/selfhost-mips/skj-cc-mips.elf
	@./build/skj-run build/selfhost-mips/skj-cc-mips.elf -o build/selfhost-mips/add.skj.s \
	    tests/selfhost/add.c 2>build/selfhost-mips/skj.err \
	    || { echo "FAIL: skj-run of in-guest skj-cc"; cat build/selfhost-mips/skj.err; exit 1; }
	@$(QEMU_MIPS) build/selfhost-mips/skj-cc-mips.elf -o build/selfhost-mips/add.qemu.s \
	    tests/selfhost/add.c 2>build/selfhost-mips/qemu.err \
	    || { echo "FAIL: qemu of in-guest skj-cc"; cat build/selfhost-mips/qemu.err; exit 1; }
	@cmp -s build/selfhost-mips/add.skj.s build/selfhost-mips/add.qemu.s \
	    || { echo "FAIL: in-guest asm differs skj-run vs qemu"; exit 1; }
	@./build/skj-as-mips -o build/selfhost-mips/add.o build/selfhost-mips/add.skj.s
	@./build/skj-ld-mips -o build/selfhost-mips/add.elf build/lib/mips/crt0.o \
	    build/selfhost-mips/add.o build/lib/mips/libc.a
	@./build/skj-run build/selfhost-mips/add.elf; rc=$$?; \
	 [ $$rc = 42 ] || { echo "FAIL: in-guest-compiled program exit $$rc (want 42)"; exit 1; }
	@echo "PASS: skj-cc self-hosts under skj-run (compiled add.c in-guest -> 42, MIPS)"

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

# C-compiler test suite runner.  Every cross-target check-cc-* suite invokes
# tests/run-cc-tests.sh with the same eight environment variables; only the
# values differ.  $(strip) neutralises the whitespace $(call) leaves around
# positional arguments, and also folds the double space an empty *FLAGS var
# leaves in an assembler command.  Argument order:
#   1 CCBIN  2 ASM  3 LD  4 QEMU  5 START  6 HALF  7 OUTDIR  8 EXCLUDE  9 EXTRAENV
define run_cc_suite
	@$(strip $(9)) CCBIN="$(strip $(1))" ASM="$(strip $(2))" LD="$(strip $(3))" \
	 QEMU="$(strip $(4))" START="$(strip $(5))" HALF="$(strip $(6))" \
	 OUTDIR="$(strip $(7))" EXCLUDE="$(strip $(8))" \
	 sh tests/run-cc-tests.sh
endef

check-cc: build/skj-cc build/skj-as build/start.o build/half.o build/va_cf.o
	@OUTDIR="$(CURDIR)/build/cc-cf" EXCLUDE="$(CF_EXCLUDE)" \
	 HALF="$(CURDIR)/build/half.o $(CURDIR)/build/va_cf.o" \
	 sh tests/run-cc-tests.sh

## C compiler tests on x86-64 (skj-cc-x86-64, qemu-x86_64).  This is where
## native single-precision float rounding is actually exercised.
check-cc-x86-64: build/skj-cc-x86-64 build/x86_64/start_sysv.o build/x86_64/half.o build/x86_64/va.o
	$(call run_cc_suite,$(CURDIR)/build/skj-cc-x86-64,$(X64_ASM) $(X64_ASMFLAGS) -o,$(X64_LD) -m elf_x86_64 -o,$(QEMU_X64),$(CURDIR)/build/x86_64/start_sysv.o,$(CURDIR)/build/x86_64/half.o $(CURDIR)/build/x86_64/va.o,$(CURDIR)/build/cc-x86_64,)

## C compiler tests on arm64 (skj-cc-arm64, qemu-aarch64).  _Float16 is a
## native fcvt here, so no half.o is linked (HALF empty).
check-cc-arm64: build/skj-cc-arm64 build/arm64/start_aapcs.o build/arm64/va.o
	$(call run_cc_suite,$(CURDIR)/build/skj-cc-arm64,$(A64_AS) -o,$(A64_LD) -o,$(QEMU_A64),$(CURDIR)/build/arm64/start_aapcs.o,$(CURDIR)/build/arm64/va.o,$(CURDIR)/build/cc-arm64,)

## C compiler tests on RISC-V (skj-cc-rv, qemu-riscv32).  The RV backend has
## hardware float (RV32FD) plus Zfh for _Float16, so the full cc suite runs;
## the test objects assemble as rv32imfd_zfh (start.o stays rv32im, a subset
## the linker merges).
check-cc-rv: build/skj-cc-rv build/rv/start.o build/rv/va_rv.o
	$(call run_cc_suite,$(CURDIR)/build/skj-cc-rv,$(RV_AS) -march=rv32imfd_zfh -mabi=ilp32 -o,$(RV_LD) -m elf32lriscv -o,$(QEMU_RV),$(CURDIR)/build/rv/start.o,$(CURDIR)/build/rv/va_rv.o,$(CURDIR)/build/cc-rv,$(RV_EXCLUDE))

## The same C suite, but assembled and linked entirely by the in-tree RV32
## toolchain (skj-as-rv + skj-ld-rv) over a skj-assembled crt, run under
## qemu-riscv32.  This is the end-to-end check that skj-as-rv/skj-ld-rv can
## build a full program with its runtime, not just match GNU as byte-for-byte.
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
	$(call run_cc_suite,$(CURDIR)/build/skj-cc-rv,$(CURDIR)/build/skj-as-rv -o,$(CURDIR)/build/skj-ld-rv -o,$(QEMU_RV),$(CURDIR)/build/rv-skj/start.o,$(CURDIR)/build/rv-skj/va_rv.o,$(CURDIR)/build/cc-rv-skj,$(RV_EXCLUDE))

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

## The SKJ_SYS_SPAWNV argv round-trip: a C root (skj-cc-rv) spawns a child with
## an argv vector through the size-prefixed spawn request, and the child echoes
## what it received.  This pins argv passing across the spawn boundary and the
## crt delivering argc/argv to main, the groundwork for the jsh userland.
build/spawn-argv/child.o: tests/spawn-argv/child.c build/skj-cc-rv build/skj-as-rv | build/spawn-argv
	./build/skj-cc-rv -o build/spawn-argv/child.s $<
	./build/skj-as-rv -o $@ build/spawn-argv/child.s

build/spawn-argv/child.elf: build/spawn-argv/child.o build/rv-skj/start.o build/skj-ld-rv
	./build/skj-ld-rv -o $@ build/rv-skj/start.o build/spawn-argv/child.o

build/spawn-argv/child_image.h: build/spawn-argv/child.elf
	printf 'static const unsigned char child_img[] = {\n' > $@
	xxd -i < build/spawn-argv/child.elf >> $@
	printf '};\n#define CHILD_IMG_LEN %uu\n' "$$(wc -c < build/spawn-argv/child.elf)" >> $@

build/spawn-argv/parent.o: tests/spawn-argv/parent.c build/spawn-argv/child_image.h build/skj-cc-rv build/skj-as-rv | build/spawn-argv
	./build/skj-cc-rv -Ibuild/spawn-argv -o build/spawn-argv/parent.s $<
	./build/skj-as-rv -o $@ build/spawn-argv/parent.s

build/spawn-argv/parent.elf: build/spawn-argv/parent.o build/rv-skj/start.o build/skj-ld-rv
	./build/skj-ld-rv -o $@ build/rv-skj/start.o build/spawn-argv/parent.o

test-rv-spawn-argv: build/spawn-argv/parent.elf build/skj-run
	@sh tests/run-spawn-argv.sh "$(CURDIR)/build/skj-run" "$(CURDIR)/build/spawn-argv/parent.elf"

## SKJ_SYS_WAIT: a C root spawns a child, blocks in wait() until it exits, and
## reads back its status.  The parent reaches wait() before the child runs, so
## this pins the park/wake path and the zombie-then-reap flow as well as the
## status round-trip.
build/spawn-wait/child.o: tests/spawn-wait/child.c build/skj-cc-rv build/skj-as-rv | build/spawn-wait
	./build/skj-cc-rv -o build/spawn-wait/child.s $<
	./build/skj-as-rv -o $@ build/spawn-wait/child.s

build/spawn-wait/child.elf: build/spawn-wait/child.o build/rv-skj/start.o build/skj-ld-rv
	./build/skj-ld-rv -o $@ build/rv-skj/start.o build/spawn-wait/child.o

build/spawn-wait/child_image.h: build/spawn-wait/child.elf
	printf 'static const unsigned char child_img[] = {\n' > $@
	xxd -i < build/spawn-wait/child.elf >> $@
	printf '};\n#define CHILD_IMG_LEN %uu\n' "$$(wc -c < build/spawn-wait/child.elf)" >> $@

build/spawn-wait/parent.o: tests/spawn-wait/parent.c build/spawn-wait/child_image.h build/skj-cc-rv build/skj-as-rv | build/spawn-wait
	./build/skj-cc-rv -Ibuild/spawn-wait -o build/spawn-wait/parent.s $<
	./build/skj-as-rv -o $@ build/spawn-wait/parent.s

build/spawn-wait/parent.elf: build/spawn-wait/parent.o build/rv-skj/start.o build/skj-ld-rv
	./build/skj-ld-rv -o $@ build/rv-skj/start.o build/spawn-wait/parent.o

test-rv-spawn-wait: build/spawn-wait/parent.elf build/skj-run
	@sh tests/run-spawn-wait.sh "$(CURDIR)/build/skj-run" "$(CURDIR)/build/spawn-wait/parent.elf"

## SKJ_SYS_KILL: a C root spawns a child, kills it with a chosen status before
## it runs, then waits and reads that status back.  Pins kill setting a wait
## status and the kill/wait/reap flow; the child must never run.
build/spawn-kill/child.o: tests/spawn-kill/child.c build/skj-cc-rv build/skj-as-rv | build/spawn-kill
	./build/skj-cc-rv -o build/spawn-kill/child.s $<
	./build/skj-as-rv -o $@ build/spawn-kill/child.s

build/spawn-kill/child.elf: build/spawn-kill/child.o build/rv-skj/start.o build/skj-ld-rv
	./build/skj-ld-rv -o $@ build/rv-skj/start.o build/spawn-kill/child.o

build/spawn-kill/child_image.h: build/spawn-kill/child.elf
	printf 'static const unsigned char child_img[] = {\n' > $@
	xxd -i < build/spawn-kill/child.elf >> $@
	printf '};\n#define CHILD_IMG_LEN %uu\n' "$$(wc -c < build/spawn-kill/child.elf)" >> $@

build/spawn-kill/parent.o: tests/spawn-kill/parent.c build/spawn-kill/child_image.h build/skj-cc-rv build/skj-as-rv | build/spawn-kill
	./build/skj-cc-rv -Ibuild/spawn-kill -o build/spawn-kill/parent.s $<
	./build/skj-as-rv -o $@ build/spawn-kill/parent.s

build/spawn-kill/parent.elf: build/spawn-kill/parent.o build/rv-skj/start.o build/skj-ld-rv
	./build/skj-ld-rv -o $@ build/rv-skj/start.o build/spawn-kill/parent.o

test-rv-spawn-kill: build/spawn-kill/parent.elf build/skj-run
	@sh tests/run-spawn-kill.sh "$(CURDIR)/build/skj-run" "$(CURDIR)/build/spawn-kill/parent.elf"

## Multi-process DWARF symbolization: a root spawns a child (built -g, embedded)
## that faults in a named function.  skj-run now retains each process's own ELF
## image, so the child's fault backtrace ("[proc 2]") is symbolized, not raw
## addresses.  The child image carries DWARF; the assertion checks a real frame
## name appears below the "[proc 2] backtrace:" header.
build/spawn-backtrace/child.o: tests/spawn-backtrace/child.c build/skj-cc-rv build/skj-as-rv | build/spawn-backtrace
	./build/skj-cc-rv -g -o build/spawn-backtrace/child.s $<
	./build/skj-as-rv -o $@ build/spawn-backtrace/child.s

build/spawn-backtrace/child.elf: build/spawn-backtrace/child.o build/rv-skj/start.o build/skj-ld-rv
	./build/skj-ld-rv -o $@ build/rv-skj/start.o build/spawn-backtrace/child.o

build/spawn-backtrace/child_image.h: build/spawn-backtrace/child.elf
	printf 'static const unsigned char child_img[] = {\n' > $@
	xxd -i < build/spawn-backtrace/child.elf >> $@
	printf '};\n#define CHILD_IMG_LEN %uu\n' "$$(wc -c < build/spawn-backtrace/child.elf)" >> $@

build/spawn-backtrace/parent.o: tests/spawn-backtrace/parent.c build/spawn-backtrace/child_image.h build/skj-cc-rv build/skj-as-rv | build/spawn-backtrace
	./build/skj-cc-rv -Ibuild/spawn-backtrace -o build/spawn-backtrace/parent.s $<
	./build/skj-as-rv -o $@ build/spawn-backtrace/parent.s

build/spawn-backtrace/parent.elf: build/spawn-backtrace/parent.o build/rv-skj/start.o build/skj-ld-rv
	./build/skj-ld-rv -o $@ build/rv-skj/start.o build/spawn-backtrace/parent.o

test-rv-spawn-backtrace: build/spawn-backtrace/parent.elf build/skj-run
	@./build/skj-run build/spawn-backtrace/parent.elf \
	    >build/spawn-backtrace/out 2>build/spawn-backtrace/err; \
	 grep -q "\[proc 2\] backtrace:" build/spawn-backtrace/err \
	    || { echo "FAIL: no child (proc 2) backtrace"; cat build/spawn-backtrace/err; exit 1; }; \
	 grep -qE '  (crash|main)( at |$$)' build/spawn-backtrace/err \
	    || { echo "FAIL: child backtrace not symbolized"; cat build/spawn-backtrace/err; exit 1; }; \
	 echo "PASS: spawned child fault is DWARF-symbolized ([proc 2] backtrace)"

## SKJ_SPAWN_FLAG_SHARE_MOUNTS: a root granted a --mount spawns a child that
## inherits the mount and reads a seeded file through it.  The runner builds
## both guests with the fs/-interpose libc, embeds the child in the parent, and
## runs the parent under skj-run with the data mount granted.  rv and cf cover
## both freestanding VM tiers (one little-endian, one big-endian guest).
test-rv-spawn-mounts: build/skj-cc-rv build/skj-as-rv build/skj-ld-rv build/rv/start.o build/rv/va_rv.o build/rv/soft64_div.o build/rv/co_switch.o build/skj-run
	@CCBIN="$(CURDIR)/build/skj-cc-rv" \
	 ASM="$(CURDIR)/build/skj-as-rv -o" \
	 LD="$(CURDIR)/build/skj-ld-rv -o" \
	 START="$(CURDIR)/build/rv/start.o" \
	 HALF="$(CURDIR)/build/rv/va_rv.o $(CURDIR)/build/rv/soft64_div.o $(CURDIR)/build/rv/co_switch.o" \
	 OUTDIR="$(CURDIR)/build/spawn-mounts-rv" \
	 RUN="$(CURDIR)/build/skj-run" \
	 NAME=test-rv-spawn-mounts \
	 sh tests/run-spawn-mounts.sh

test-cf-spawn-mounts: build/skj-cc build/skj-as build/skj-ld build/start.o build/half.o build/va_cf.o build/co_switch_cf.o build/skj-run
	@CCBIN="$(CURDIR)/build/skj-cc" \
	 ASM="$(CURDIR)/build/skj-as -o" \
	 LD="$(CURDIR)/build/skj-ld -o" \
	 START="$(CURDIR)/build/start.o" \
	 HALF="$(CURDIR)/build/half.o $(CURDIR)/build/va_cf.o $(CURDIR)/build/co_switch_cf.o" \
	 OUTDIR="$(CURDIR)/build/spawn-mounts-cf" \
	 RUN="$(CURDIR)/build/skj-run" \
	 NAME=test-cf-spawn-mounts \
	 sh tests/run-spawn-mounts.sh

## SKJ_SYS_PIPE + fd passing: a root wires a pipe between a writer and a reader
## child (the ends placed through the spawn fd map), so a message crosses the
## pipe between two sandboxes.  rv and cf cover both freestanding VM tiers.
test-rv-spawn-pipe: build/skj-cc-rv build/skj-as-rv build/skj-ld-rv build/rv/start.o build/rv/va_rv.o build/rv/soft64_div.o build/rv/co_switch.o build/skj-run
	@CCBIN="$(CURDIR)/build/skj-cc-rv" \
	 ASM="$(CURDIR)/build/skj-as-rv -o" \
	 LD="$(CURDIR)/build/skj-ld-rv -o" \
	 START="$(CURDIR)/build/rv/start.o" \
	 HALF="$(CURDIR)/build/rv/va_rv.o $(CURDIR)/build/rv/soft64_div.o $(CURDIR)/build/rv/co_switch.o" \
	 OUTDIR="$(CURDIR)/build/spawn-pipe-rv" \
	 RUN="$(CURDIR)/build/skj-run" \
	 NAME=test-rv-spawn-pipe \
	 sh tests/run-spawn-pipe.sh

test-cf-spawn-pipe: build/skj-cc build/skj-as build/skj-ld build/start.o build/half.o build/va_cf.o build/co_switch_cf.o build/skj-run
	@CCBIN="$(CURDIR)/build/skj-cc" \
	 ASM="$(CURDIR)/build/skj-as -o" \
	 LD="$(CURDIR)/build/skj-ld -o" \
	 START="$(CURDIR)/build/start.o" \
	 HALF="$(CURDIR)/build/half.o $(CURDIR)/build/va_cf.o $(CURDIR)/build/co_switch_cf.o" \
	 OUTDIR="$(CURDIR)/build/spawn-pipe-cf" \
	 RUN="$(CURDIR)/build/skj-run" \
	 NAME=test-cf-spawn-pipe \
	 sh tests/run-spawn-pipe.sh

## The jsh userland: the jsh shell plus the coreutils (cat, echo, ls, rm, sleep,
## cp, mv), built as guest ELFs over the fs-interpose libc.  The coreutils are
## packed into a romfs mounted at bin:, jsh resolves a bare command name there
## and spawns a fresh sandbox per command, wiring pipes and redirects through
## the spawn fd map.  rv and cf cover both freestanding VM tiers.
test-rv-jsh: build/skj-cc-rv build/skj-as-rv build/skj-ld-rv build/skj-ar build/rv/start.o build/rv/va_rv.o build/rv/soft64_div.o build/rv/co_switch.o build/rv/setjmp.o build/skj-run
	@CCBIN="$(CURDIR)/build/skj-cc-rv" \
	 ASM="$(CURDIR)/build/skj-as-rv -o" \
	 LD="$(CURDIR)/build/skj-ld-rv -o" \
	 AR="$(CURDIR)/build/skj-ar" \
	 START="$(CURDIR)/build/rv/start.o" \
	 HALF="$(CURDIR)/build/rv/va_rv.o $(CURDIR)/build/rv/soft64_div.o $(CURDIR)/build/rv/co_switch.o $(CURDIR)/build/rv/setjmp.o" \
	 OUTDIR="$(CURDIR)/build/jsh-rv" \
	 RUN="$(CURDIR)/build/skj-run" \
	 NAME=test-rv-jsh \
	 sh tests/run-jsh.sh

## test-rv-ps: the standalone ps tool over the always-on proc: pseudo fs.  Runs
## ps as the sole process under skj-run (no --mount); it must list pid 1 running.
test-rv-ps: build/skj-cc-rv build/skj-as-rv build/skj-ld-rv build/skj-ar build/rv/start.o build/rv/va_rv.o build/rv/soft64_div.o build/rv/co_switch.o build/rv/setjmp.o build/skj-run
	@CCBIN="$(CURDIR)/build/skj-cc-rv" \
	 ASM="$(CURDIR)/build/skj-as-rv -o" \
	 LD="$(CURDIR)/build/skj-ld-rv -o" \
	 AR="$(CURDIR)/build/skj-ar" \
	 START="$(CURDIR)/build/rv/start.o" \
	 HALF="$(CURDIR)/build/rv/va_rv.o $(CURDIR)/build/rv/soft64_div.o $(CURDIR)/build/rv/co_switch.o $(CURDIR)/build/rv/setjmp.o" \
	 OUTDIR="$(CURDIR)/build/ps-rv" \
	 RUN="$(CURDIR)/build/skj-run" \
	 NAME=test-rv-ps \
	 sh tests/run-ps.sh

test-cf-jsh: build/skj-cc build/skj-as build/skj-ld build/skj-ar build/start.o build/half.o build/va_cf.o build/co_switch_cf.o build/setjmp_cf.o build/skj-run
	@CCBIN="$(CURDIR)/build/skj-cc" \
	 ASM="$(CURDIR)/build/skj-as -o" \
	 LD="$(CURDIR)/build/skj-ld -o" \
	 AR="$(CURDIR)/build/skj-ar" \
	 START="$(CURDIR)/build/start.o" \
	 HALF="$(CURDIR)/build/half.o $(CURDIR)/build/va_cf.o $(CURDIR)/build/co_switch_cf.o $(CURDIR)/build/setjmp_cf.o" \
	 OUTDIR="$(CURDIR)/build/jsh-cf" \
	 RUN="$(CURDIR)/build/skj-run" \
	 NAME=test-cf-jsh \
	 sh tests/run-jsh.sh

test-mips-jsh: build/skj-cc-mips build/skj-as-mips build/skj-ld-mips build/skj-ar build/mips/start.o build/mips/half.o build/mips/va_mips.o build/mips/soft64_div.o build/mips/co_switch.o build/mips/setjmp.o build/skj-run
	@CCBIN="$(CURDIR)/build/skj-cc-mips" \
	 ASM="$(CURDIR)/build/skj-as-mips -o" \
	 LD="$(CURDIR)/build/skj-ld-mips -o" \
	 AR="$(CURDIR)/build/skj-ar" \
	 START="$(CURDIR)/build/mips/start.o" \
	 HALF="$(CURDIR)/build/mips/half.o $(CURDIR)/build/mips/va_mips.o $(CURDIR)/build/mips/soft64_div.o $(CURDIR)/build/mips/co_switch.o $(CURDIR)/build/mips/setjmp.o" \
	 OUTDIR="$(CURDIR)/build/jsh-mips" \
	 RUN="$(CURDIR)/build/skj-run" \
	 NAME=test-mips-jsh \
	 sh tests/run-jsh.sh

## The LP64 hardware-virtualization tiers run jsh too: the same shell + coreutils
## over the fs-interpose libc, built by the x86-64 / arm64 toolchain, linked with
## the HV crt and seam (plus the LP64 setjmp), spawned per command under skj-run.
## x86-64 runs the guest where /dev/kvm is reachable and skips cleanly otherwise;
## arm64 can only run on an aarch64 KVM host, so it verifies the build everywhere
## and runs the guest only there.
test-x86-64-jsh: $(VMM_PROBE_DEPS) build/skj-cc-x86-64 build/skj-as-x86-64 \
                 build/hv-x86_64/start.o build/hv-x86_64/syscall.o build/hv-x86_64/setjmp.o \
                 build/x86_64/half.o build/x86_64/va.o build/x86_64/soft64.o build/x86_64/co_switch.o \
                 build/skj-run
	@if ! $(vmm_works); then echo "SKIP: x86-64 jsh (no functional KVM vcpu)"; exit 0; fi; \
	 CCBIN="$(CURDIR)/build/skj-cc-x86-64" \
	 ASM="$(CURDIR)/build/skj-as-x86-64 -o" \
	 LD="$(X64_LD) -m elf_x86_64 -e _start -Ttext=0x100000 --build-id=none -o" \
	 AR="ar" \
	 START="$(CURDIR)/build/hv-x86_64/start.o" \
	 HALF="$(CURDIR)/build/hv-x86_64/syscall.o $(CURDIR)/build/x86_64/va.o $(CURDIR)/build/x86_64/half.o $(CURDIR)/build/x86_64/soft64.o $(CURDIR)/build/x86_64/co_switch.o $(CURDIR)/build/hv-x86_64/setjmp.o" \
	 OUTDIR="$(CURDIR)/build/jsh-x86_64" \
	 RUN="$(CURDIR)/build/skj-run" \
	 NAME=test-x86-64-jsh \
	 sh tests/run-jsh.sh

## The cooperative-park stress test on the x86-64 HV tier: two guests share one
## host thread and move a multi-pipe-buffer payload, so a full-pipe write
## (POLLOUT), an empty-pipe read (POLLIN), a sched_yield, and a nanosleep each
## park the caller and let the sibling run instead of blocking the shared thread
## (the HV syscall suspend/resume path; see emu/hvcore.c and emu/guest.c).  SKIPs
## without an accessible /dev/kvm, like the other HV tests.
test-hv-x86-64-pump: $(VMM_PROBE_DEPS) build/skj-cc-x86-64 build/skj-as-x86-64 \
                 build/hv-x86_64/start.o build/hv-x86_64/syscall.o build/hv-x86_64/setjmp.o \
                 build/x86_64/half.o build/x86_64/va.o build/x86_64/soft64.o build/x86_64/co_switch.o \
                 build/skj-run
	@if ! $(vmm_works); then echo "SKIP: x86-64 hv-pump (no functional KVM vcpu)"; exit 0; fi; \
	 CCBIN="$(CURDIR)/build/skj-cc-x86-64" \
	 ASM="$(CURDIR)/build/skj-as-x86-64 -o" \
	 LD="$(X64_LD) -m elf_x86_64 -e _start -Ttext=0x100000 --build-id=none -o" \
	 START="$(CURDIR)/build/hv-x86_64/start.o" \
	 HALF="$(CURDIR)/build/hv-x86_64/syscall.o $(CURDIR)/build/x86_64/va.o $(CURDIR)/build/x86_64/half.o $(CURDIR)/build/x86_64/soft64.o $(CURDIR)/build/hv-x86_64/setjmp.o" \
	 OUTDIR="$(CURDIR)/build/hv-pump-x86_64" \
	 RUN="$(CURDIR)/build/skj-run" \
	 NAME=test-hv-x86-64-pump \
	 sh tests/run-hv-pump.sh

test-arm64-jsh: build/skj-cc-arm64 build/skj-as-aarch64 \
                build/hv-arm64/start.o build/hv-arm64/syscall.o build/hv-arm64/setjmp.o \
                build/arm64/va.o build/arm64/soft64.o build/arm64/co_switch.o \
                build/skj-run
	@if [ "$$(uname -m)" = "aarch64" ] && [ -r /dev/kvm ] && [ -w /dev/kvm ]; then B=; else B=1; fi; \
	 CCBIN="$(CURDIR)/build/skj-cc-arm64" \
	 ASM="$(CURDIR)/build/skj-as-aarch64 -o" \
	 LD="$(A64_LD) -e _start -Ttext=0x100000 --build-id=none -o" \
	 AR="ar" \
	 START="$(CURDIR)/build/hv-arm64/start.o" \
	 HALF="$(CURDIR)/build/hv-arm64/syscall.o $(CURDIR)/build/arm64/va.o $(CURDIR)/build/arm64/soft64.o $(CURDIR)/build/arm64/co_switch.o $(CURDIR)/build/hv-arm64/setjmp.o" \
	 OUTDIR="$(CURDIR)/build/jsh-arm64" \
	 RUN="$(CURDIR)/build/skj-run" \
	 NAME=test-arm64-jsh \
	 BUILD_ONLY="$$B" \
	 sh tests/run-jsh.sh

## The vDSO injection seam: the host maps a small image of pure leaf functions
## (runtime/vdso.c, linked at a fixed guest address) into a guest and advertises
## its table through the auxv tag AT_SKJ_VDSO; the guest finds it by walking its
## auxv and calls the functions through the export table.  One image is built
## once and shared read-only across every sandbox: the emulated ILP32 cores
## alias it through gm_page, the LP64 (HV) cores map it as a read-only
## hypervisor memory slot.  Each target builds the image with its own toolchain:
## ELF32 at 0x50000000 for the emulated cores (RISC-V, ColdFire, MIPS), ELF64 at
## SKJ_HV_VDSO (0x20000000) for the HV guests.
build/rv/vdso.o: runtime/vdso.c include/skj_vdso.h build/skj-cc-rv build/skj-as-rv | build/rv
	@./build/skj-cc-rv -Iinclude -o build/rv/vdso.s runtime/vdso.c
	@./build/skj-as-rv -o $@ build/rv/vdso.s

build/rv/vdso.elf: build/rv/vdso.o runtime/vdso_rv.ld build/skj-ld-rv
	@./build/skj-ld-rv -T runtime/vdso_rv.ld -o $@ build/rv/vdso.o

build/cf/vdso.o: runtime/vdso.c include/skj_vdso.h build/skj-cc build/skj-as | build/cf
	@./build/skj-cc -Iinclude -o build/cf/vdso.s runtime/vdso.c
	@./build/skj-as -o $@ build/cf/vdso.s

build/cf/vdso.elf: build/cf/vdso.o runtime/vdso_cf.ld build/skj-ld
	@./build/skj-ld -T runtime/vdso_cf.ld -o $@ build/cf/vdso.o

build/mips/vdso.o: runtime/vdso.c include/skj_vdso.h build/skj-cc-mips build/skj-as-mips | build/mips
	@./build/skj-cc-mips -Iinclude -o build/mips/vdso.s runtime/vdso.c
	@./build/skj-as-mips -o $@ build/mips/vdso.s

build/mips/vdso.elf: build/mips/vdso.o runtime/vdso_mips.ld build/skj-ld-mips
	@./build/skj-ld-mips -T runtime/vdso_mips.ld -o $@ build/mips/vdso.o

build/x86_64/vdso.o: runtime/vdso.c include/skj_vdso.h build/skj-cc-x86-64 $(X64_ASM_DEP) | build/x86_64
	@./build/skj-cc-x86-64 -Iinclude -o build/x86_64/vdso.s runtime/vdso.c
	@$(X64_ASM) $(X64_ASMFLAGS) -o $@ build/x86_64/vdso.s

build/x86_64/vdso.elf: build/x86_64/vdso.o runtime/vdso_x86_64.ld
	@$(X64_LD) -m elf_x86_64 -T runtime/vdso_x86_64.ld --build-id=none -o $@ build/x86_64/vdso.o

build/arm64/vdso.o: runtime/vdso.c include/skj_vdso.h build/skj-cc-arm64 $(A64_ASM_DEP) | build/arm64
	@./build/skj-cc-arm64 -Iinclude -o build/arm64/vdso.s runtime/vdso.c
	@$(A64_AS) -o $@ build/arm64/vdso.s

build/arm64/vdso.elf: build/arm64/vdso.o runtime/vdso_arm64.ld
	@$(A64_LD) -T runtime/vdso_arm64.ld --build-id=none -o $@ build/arm64/vdso.o

test-rv-vdso: build/skj-cc-rv build/skj-as-rv build/skj-ld-rv build/rv/start.o \
              build/rv/va_rv.o build/rv/soft64_div.o build/lib/riscv/libc.a \
              build/rv/vdso.elf build/skj-run examples/vdso_test.c include/skj_vdso.h
	@./build/skj-cc-rv -Iinclude -Ilibc -o build/rv/vdso_test.s examples/vdso_test.c
	@./build/skj-as-rv -o build/rv/vdso_test.o build/rv/vdso_test.s
	@./build/skj-ld-rv -o build/rv/vdso_test.elf build/rv/start.o build/rv/vdso_test.o \
	    build/lib/riscv/libc.a build/rv/va_rv.o build/rv/soft64_div.o
	@out=$$(./build/skj-run --vdso build/rv/vdso.elf build/rv/vdso_test.elf); \
	 if [ "$$out" = "vdso ok" ]; then \
	     echo "PASS: vDSO seam (rv): guest called host-injected leaf functions"; \
	 else \
	     echo "FAIL: test-rv-vdso got '$$out'"; exit 1; \
	 fi

## The in-memory root seam: skj-run --mem-root reads the ELF into a buffer and runs
## it from there (sched_root.image), the way an embedding host whose guest images
## are byte buffers does, rather than opening the path.  Prove it behaves exactly
## like the file path: same stdout, same exit, on one guest run both ways.
test-rv-mem-root: build/skj-cc-rv build/skj-as-rv build/skj-ld-rv build/rv/start.o \
                  build/rv/va_rv.o build/rv/soft64_div.o build/lib/riscv/libc.a \
                  build/skj-run tests/mem-root/hello.c | build/rv
	@./build/skj-cc-rv -Ilibc -o build/rv/memroot.s tests/mem-root/hello.c
	@./build/skj-as-rv -o build/rv/memroot.o build/rv/memroot.s
	@./build/skj-ld-rv -o build/rv/memroot.elf build/rv/start.o build/rv/memroot.o \
	    build/lib/riscv/libc.a build/rv/va_rv.o build/rv/soft64_div.o
	@pf=$$(./build/skj-run build/rv/memroot.elf 2>&1); pe=$$?; \
	 mf=$$(./build/skj-run --mem-root build/rv/memroot.elf 2>&1); me=$$?; \
	 if [ "$$mf" = "mem-root ok" ] && [ "$$mf" = "$$pf" ] && [ "$$me" = "0" ] && [ "$$me" = "$$pe" ]; then \
	     echo "PASS: in-memory root (--mem) matches the file path"; \
	 else \
	     echo "FAIL: test-rv-mem-root path='$$pf'($$pe) mem='$$mf'($$me)"; exit 1; \
	 fi

## The embedding seam a complex host (boris, smolmoo) uses directly: get the
## arch's ops from the public getter (skj_ops_rv32), init a scheduler, add a root
## from an in-memory image, run it, read the exit through S.procs[].  No
## run_sandbox, no CLI.  Links the emu objects (minus the CLI main) plus the
## driver; reuses build/rv/memroot.elf from test-rv-mem-root as the guest.
test-sched-embed: build/skj-run tests/sched_embed.c tests/mem-root/hello.c $(EMU_HDR) \
                  build/skj-cc-rv build/skj-as-rv build/skj-ld-rv build/rv/start.o \
                  build/rv/va_rv.o build/rv/soft64_div.o build/lib/riscv/libc.a | build/rv
	@./build/skj-cc-rv -Ilibc -o build/rv/memroot.s tests/mem-root/hello.c
	@./build/skj-as-rv -o build/rv/memroot.o build/rv/memroot.s
	@./build/skj-ld-rv -o build/rv/memroot.elf build/rv/start.o build/rv/memroot.o \
	    build/lib/riscv/libc.a build/rv/va_rv.o build/rv/soft64_div.o
	@# the host harness is named *_test so run-tests.sh skips it: it is an x86-64
	@# host binary, not an rv32 guest, and build/rv is scanned by check-rv (a stale
	@# build/rv/sched_embed otherwise ran under qemu-riscv32 and "failed").
	@$(CC) $(CFLAGS) $(EMU_FLAGS) -Iemu -o build/rv/sched_embed_test \
	    tests/sched_embed.c $(filter-out emu/main.c,$(EMU_SRC)) \
	    -lm $(EMU_LDLIBS) -lpthread
	@out=$$(./build/rv/sched_embed_test build/rv/memroot.elf 2>&1); \
	 if echo "$$out" | grep -qx "mem-root ok" && echo "$$out" | grep -qx "embed ok"; then \
	     echo "PASS: embedding API (ops getter + in-memory root) drives a guest"; \
	 else \
	     echo "FAIL: test-sched-embed got '$$out'"; exit 1; \
	 fi

## The same emulated-tier seam on the other two ILP32 cores.  ColdFire is
## big-endian, MIPS little-endian, so these also prove the loader and the
## system-page writer honour the guest byte order.  The driver (skjcc) builds
## the test program with its own libc; the vDSO image is linked once at
## 0x50000000 and aliased read-only through gm_page.
test-cf-vdso: build/skjcc $(DRIVER_LIB_CF) build/cf/vdso.elf build/skj-run \
              examples/vdso_test.c include/skj_vdso.h | build/env-test
	@./build/skjcc -target coldfire -Iinclude -o build/env-test/vdso.cf examples/vdso_test.c
	@out=$$(./build/skj-run --vdso build/cf/vdso.elf build/env-test/vdso.cf 2>&1); \
	 if [ "$$out" = "vdso ok" ]; then \
	     echo "PASS: vDSO seam (cf): big-endian guest called host-injected leaf functions"; \
	 else \
	     echo "FAIL: test-cf-vdso got '$$out'"; exit 1; \
	 fi

test-mips-vdso: build/skjcc $(DRIVER_LIB_MIPS) build/mips/vdso.elf build/skj-run \
                examples/vdso_test.c include/skj_vdso.h | build/env-test
	@./build/skjcc -target mips -Iinclude -o build/env-test/vdso.mips examples/vdso_test.c
	@out=$$(./build/skj-run --vdso build/mips/vdso.elf build/env-test/vdso.mips 2>&1); \
	 if [ "$$out" = "vdso ok" ]; then \
	     echo "PASS: vDSO seam (mips): guest called host-injected leaf functions"; \
	 else \
	     echo "FAIL: test-mips-vdso got '$$out'"; exit 1; \
	 fi

## The same seam on the LP64 HV tier: the vDSO is an ELF64 image backing a
## read-only hypervisor memory slot at SKJ_HV_VDSO, shared across every VM.  The
## guest links the syscall seam's weak write plus getenv.o/string.o for environ
## (the crt sets environ from the envp the scheduler laid down); no stdio or fs.
test-hv-x86-64-vdso: $(VMM_PROBE_DEPS) build/skj-cc-x86-64 build/skj-as-x86-64 build/skj-run \
                     build/hv-x86_64/start.o build/hv-x86_64/syscall.o \
                     build/x86_64/vdso.elf examples/vdso_test.c libc/getenv.c \
                     libc/string.c include/skj_vdso.h $(X64_ASM_DEP) | build/x86_64
	@if ! $(vmm_works); then echo "SKIP: hv x86-64 vdso (no functional KVM vcpu)"; exit 0; fi; \
	 ./build/skj-cc-x86-64 -Iinclude -Ilibc -o build/x86_64/vdso_test.s examples/vdso_test.c; \
	 $(X64_ASM) $(X64_ASMFLAGS) -o build/x86_64/vdso_test.o build/x86_64/vdso_test.s; \
	 ./build/skj-cc-x86-64 -Ilibc -o build/x86_64/vdso_getenv.s libc/getenv.c; \
	 $(X64_ASM) $(X64_ASMFLAGS) -o build/x86_64/vdso_getenv.o build/x86_64/vdso_getenv.s; \
	 ./build/skj-cc-x86-64 -Ilibc -o build/x86_64/vdso_string.s libc/string.c; \
	 $(X64_ASM) $(X64_ASMFLAGS) -o build/x86_64/vdso_string.o build/x86_64/vdso_string.s; \
	 $(X64_LD) -m elf_x86_64 -e _start -Ttext=0x100000 --build-id=none \
	     -o build/x86_64/vdso_test.elf build/hv-x86_64/start.o build/x86_64/vdso_test.o \
	     build/x86_64/vdso_getenv.o build/x86_64/vdso_string.o build/hv-x86_64/syscall.o; \
	 out=$$(./build/skj-run --vdso build/x86_64/vdso.elf build/x86_64/vdso_test.elf 2>/dev/null); \
	 if [ "$$out" = "vdso ok" ]; then \
	     echo "PASS: vDSO seam (hv x86-64): guest called host-injected leaf functions"; \
	 else \
	     echo "FAIL: test-hv-x86-64-vdso got '$$out'"; exit 1; \
	 fi

test-hv-arm64-vdso: build/skj-cc-arm64 build/skj-as-aarch64 build/skj-run \
                    build/hv-arm64/start.o build/hv-arm64/syscall.o \
                    build/arm64/vdso.elf examples/vdso_test.c libc/getenv.c \
                    libc/string.c include/skj_vdso.h $(A64_ASM_DEP) | build/arm64
	@./build/skj-cc-arm64 -Iinclude -Ilibc -o build/arm64/vdso_test.s examples/vdso_test.c; \
	 $(A64_AS) -o build/arm64/vdso_test.o build/arm64/vdso_test.s; \
	 ./build/skj-cc-arm64 -Ilibc -o build/arm64/vdso_getenv.s libc/getenv.c; \
	 $(A64_AS) -o build/arm64/vdso_getenv.o build/arm64/vdso_getenv.s; \
	 ./build/skj-cc-arm64 -Ilibc -o build/arm64/vdso_string.s libc/string.c; \
	 $(A64_AS) -o build/arm64/vdso_string.o build/arm64/vdso_string.s; \
	 $(A64_LD) -e _start -Ttext=0x100000 --build-id=none \
	     -o build/arm64/vdso_test.elf build/hv-arm64/start.o build/arm64/vdso_test.o \
	     build/arm64/vdso_getenv.o build/arm64/vdso_string.o build/hv-arm64/syscall.o; \
	 if [ "$$(uname -m)" = "aarch64" ] && [ -r /dev/kvm ] && [ -w /dev/kvm ]; then \
	     out=$$(./build/skj-run --vdso build/arm64/vdso.elf build/arm64/vdso_test.elf 2>/dev/null); \
	     if [ "$$out" = "vdso ok" ]; then \
	         echo "PASS: vDSO seam (hv arm64): guest called host-injected leaf functions"; \
	     else \
	         echo "FAIL: test-hv-arm64-vdso got '$$out'"; exit 1; \
	     fi; \
	 else \
	     echo "PASS: vDSO seam (hv arm64): built (run skipped, no aarch64 /dev/kvm)"; \
	 fi

## Per-root environment passing: --env lays "KEY=VALUE" entries on the initial
## stack, the crt points environ at them, getenv walks it.  Run across both
## endiannesses (rv little, cf big) and both widths (ILP32 rv/cf, LP64 x86-64),
## since the stack builder writes the envp pointers in the guest's own layout.
test-rv-env: build/skjcc $(DRIVER_LIB_RV) build/skj-run examples/env_test.c | build/env-test
	@./build/skjcc -target riscv -o build/env-test/env.rv examples/env_test.c
	@out=$$(./build/skj-run --env SKJ_TEST=hello --env SKJ_OTHER=world build/env-test/env.rv 2>&1); \
	 if [ "$$out" = "env ok" ]; then \
	     echo "PASS: env passing (rv): guest read its injected environment"; \
	 else echo "FAIL: test-rv-env got '$$out'"; exit 1; fi

test-cf-env: build/skjcc $(DRIVER_LIB_CF) build/skj-run examples/env_test.c | build/env-test
	@./build/skjcc -target coldfire -o build/env-test/env.cf examples/env_test.c
	@out=$$(./build/skj-run --env SKJ_TEST=hello --env SKJ_OTHER=world build/env-test/env.cf 2>&1); \
	 if [ "$$out" = "env ok" ]; then \
	     echo "PASS: env passing (cf): big-endian guest read its injected environment"; \
	 else echo "FAIL: test-cf-env got '$$out'"; exit 1; fi

test-hv-x86-64-env: $(VMM_PROBE_DEPS) build/skj-cc-x86-64 build/skj-as-x86-64 build/skj-run \
                    build/hv-x86_64/start.o build/hv-x86_64/syscall.o \
                    examples/env_test.c libc/getenv.c libc/string.c \
                    $(X64_ASM_DEP) | build/env-test
	@if ! $(vmm_works); then echo "SKIP: hv x86-64 env (no functional KVM vcpu)"; exit 0; fi; \
	 d=build/env-test; \
	 ./build/skj-cc-x86-64 -Iinclude -Ilibc -o $$d/env_x86.s examples/env_test.c; \
	 $(X64_ASM) $(X64_ASMFLAGS) -o $$d/env_x86.o $$d/env_x86.s; \
	 ./build/skj-cc-x86-64 -Ilibc -o $$d/env_getenv.s libc/getenv.c; \
	 $(X64_ASM) $(X64_ASMFLAGS) -o $$d/env_getenv.o $$d/env_getenv.s; \
	 ./build/skj-cc-x86-64 -Ilibc -o $$d/env_string.s libc/string.c; \
	 $(X64_ASM) $(X64_ASMFLAGS) -o $$d/env_string.o $$d/env_string.s; \
	 $(X64_LD) -m elf_x86_64 -e _start -Ttext=0x100000 --build-id=none \
	     -o $$d/env.x86-64 build/hv-x86_64/start.o $$d/env_x86.o $$d/env_getenv.o \
	     $$d/env_string.o build/hv-x86_64/syscall.o; \
	 out=$$(./build/skj-run --env SKJ_TEST=hello --env SKJ_OTHER=world $$d/env.x86-64 2>&1); \
	 if [ "$$out" = "env ok" ]; then \
	     echo "PASS: env passing (hv x86-64): LP64 guest read its injected environment"; \
	 else echo "FAIL: test-hv-x86-64-env got '$$out'"; exit 1; fi

## The cross-language sandbox demo: a C root (skj-cc-rv) spawns two children,
## one built from C and one from Kobold-Pascal (skj-kpc -> skj-kpc-rv ->
## skj-as-rv -> skj-ld-rv over the freestanding crt + libkpaot-rv-free.a).  All
## three are RV32 ELFs the toolkit produced from the one shared IR, so skj-run
## schedules a C process and a Pascal process side by side with nothing
## language-specific in the runtime.  This is the companion of test-rv-sandbox,
## making the "one IR, many front ends, one runtime" claim concrete across two
## source languages at once.  Both children run the same sum 1..99 = 4950.
build/xlang-sandbox/sys.o: tests/xlang-sandbox/sys.S build/skj-as-rv | build/xlang-sandbox
	./build/skj-as-rv -o $@ $<

build/xlang-sandbox/cchild.o: tests/xlang-sandbox/cchild.c build/skj-cc-rv build/skj-as-rv | build/xlang-sandbox
	./build/skj-cc-rv -o build/xlang-sandbox/cchild.s $<
	./build/skj-as-rv -o $@ build/xlang-sandbox/cchild.s

build/xlang-sandbox/cchild.elf: build/xlang-sandbox/cchild.o build/rv-skj/start.o build/skj-ld-rv
	./build/skj-ld-rv -o $@ build/rv-skj/start.o build/xlang-sandbox/cchild.o

build/xlang-sandbox/kchild.kpm: tests/xlang-sandbox/kchild.pas build/skj-kpc-rv | build/xlang-sandbox
	./build/skj-kpc-rv --emit-kpm $< > $@

build/xlang-sandbox/kchild.o: build/xlang-sandbox/kchild.kpm build/skj-kpc-rv build/skj-as-rv
	./build/skj-kpc-rv -o build/xlang-sandbox/kchild.s $<
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

## The MIPS twin of test-rv-sandbox: the same arch-neutral parent.c/child.c, a
## MIPS sys.S of emulator-private syscall stubs, built with the in-tree MIPS
## toolchain and run under skj-run's MIPS core.  Proves the process sandbox
## (spawn, the round-robin scheduler, the guarded map/unmap, and the shared
## memory budget) is at parity with the RISC-V core.  The runner is shared.
build/sandbox-mips/sys.o: tests/sandbox-mips/sys.S build/skj-as-mips | build/sandbox-mips
	./build/skj-as-mips -o $@ $<

build/sandbox-mips/child.o: tests/sandbox/child.c build/skj-cc-mips build/skj-as-mips | build/sandbox-mips
	./build/skj-cc-mips -o build/sandbox-mips/child.s $<
	./build/skj-as-mips -o $@ build/sandbox-mips/child.s

build/sandbox-mips/child.elf: build/sandbox-mips/child.o build/sandbox-mips/sys.o build/mips/start.o build/skj-ld-mips
	./build/skj-ld-mips -o $@ build/mips/start.o build/sandbox-mips/child.o build/sandbox-mips/sys.o

build/sandbox-mips/child_image.h: build/sandbox-mips/child.elf
	printf 'static const unsigned char child_img[] = {\n' > $@
	xxd -i < build/sandbox-mips/child.elf >> $@
	printf '};\n#define CHILD_IMG_LEN %uu\n' "$$(wc -c < build/sandbox-mips/child.elf)" >> $@

build/sandbox-mips/parent.o: tests/sandbox/parent.c build/sandbox-mips/child_image.h build/skj-cc-mips build/skj-as-mips | build/sandbox-mips
	./build/skj-cc-mips -Ibuild/sandbox-mips -o build/sandbox-mips/parent.s $<
	./build/skj-as-mips -o $@ build/sandbox-mips/parent.s

build/sandbox-mips/parent.elf: build/sandbox-mips/parent.o build/sandbox-mips/sys.o build/mips/start.o build/skj-ld-mips
	./build/skj-ld-mips -o $@ build/mips/start.o build/sandbox-mips/parent.o build/sandbox-mips/sys.o

test-mips-sandbox: build/sandbox-mips/parent.elf build/skj-run
	@sh tests/run-sandbox-test.sh "$(CURDIR)/build/skj-run" "$(CURDIR)/build/sandbox-mips/parent.elf" test-mips-sandbox

## The ColdFire twin: the same arch-neutral parent.c/child.c, a ColdFire sys.S
## of TRAP #0 stubs, built with the in-tree m68k toolchain.  The ColdFire core
## services its TRAP #0 syscalls inside its run wrapper, so it drives the one
## shared scheduler the same as rv and MIPS; this exercises its half of the
## process sandbox (spawn, map/unmap, the shared budget).
build/sandbox-cf/sys.o: tests/sandbox-cf/sys.S build/skj-as | build/sandbox-cf
	./build/skj-as -o $@ $<

build/sandbox-cf/child.o: tests/sandbox/child.c build/skj-cc build/skj-as | build/sandbox-cf
	./build/skj-cc -o build/sandbox-cf/child.s $<
	./build/skj-as -o $@ build/sandbox-cf/child.s

build/sandbox-cf/child.elf: build/sandbox-cf/child.o build/sandbox-cf/sys.o build/start.o build/skj-ld
	./build/skj-ld -o $@ build/start.o build/sandbox-cf/child.o build/sandbox-cf/sys.o

build/sandbox-cf/child_image.h: build/sandbox-cf/child.elf
	printf 'static const unsigned char child_img[] = {\n' > $@
	xxd -i < build/sandbox-cf/child.elf >> $@
	printf '};\n#define CHILD_IMG_LEN %uu\n' "$$(wc -c < build/sandbox-cf/child.elf)" >> $@

build/sandbox-cf/parent.o: tests/sandbox/parent.c build/sandbox-cf/child_image.h build/skj-cc build/skj-as | build/sandbox-cf
	./build/skj-cc -Ibuild/sandbox-cf -o build/sandbox-cf/parent.s $<
	./build/skj-as -o $@ build/sandbox-cf/parent.s

build/sandbox-cf/parent.elf: build/sandbox-cf/parent.o build/sandbox-cf/sys.o build/start.o build/skj-ld
	./build/skj-ld -o $@ build/start.o build/sandbox-cf/parent.o build/sandbox-cf/sys.o

test-cf-sandbox: build/sandbox-cf/parent.elf build/skj-run
	@sh tests/run-sandbox-test.sh "$(CURDIR)/build/skj-run" "$(CURDIR)/build/sandbox-cf/parent.elf" test-cf-sandbox

## check-vendor: the only test that exercises vendor.sh.  It vendors this very
## checkout (riscv + cc + toolchain,runtime,sandbox,emulator), builds the result
## with the generated skjegg.mk, then compiles and runs a confined guest with the
## vendored driver + skj-run.  A staging or wiring gap in vendor.sh (an unstaged
## file, a missing -I, a stale source list) fails the vendored build or a case,
## so these gaps stop reaching consumers unnoticed.  Needs the RV cross tools.
check-vendor:
	@sh tests/run-vendor-smoke.sh "$(CURDIR)"

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
	$(call run_cc_suite,$(CURDIR)/build/skj-cc-rv,$(CURDIR)/build/skj-as-rv -o,$(CURDIR)/build/skj-ld-rv -o,$(QEMU_RV),$(CURDIR)/build/rv-skj/start_psabi_cc.o,$(CURDIR)/build/rv-skj/va_rv.o,$(CURDIR)/build/cc-rv-psabi,$(RV_EXCLUDE))

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
build/lib/x86-64/crt0.o: build/x86_64/start_sysv.o
	@mkdir -p $(@D) && cp $< $@
build/lib/x86-64/half.o: build/x86_64/half.o
	@mkdir -p $(@D) && cp $< $@
build/lib/x86-64/va.o: build/x86_64/va.o
	@mkdir -p $(@D) && cp $< $@
build/lib/arm64/crt0.o: build/arm64/start_aapcs.o
	@mkdir -p $(@D) && cp $< $@
build/lib/arm64/va.o: build/arm64/va.o
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
	@$(LIBC_ENV_cf) OUTDIR="$(CURDIR)/build/libc-cf" \
	 FS_INTERPOSE=1 FS_AMBIENT_DEFAULT=1 \
	 sh tests/run-libc-tests.sh

check-libc-rv: build/skj-cc-rv build/rv/start.o build/rv/va_rv.o build/rv/soft64_div.o build/rv/co_switch.o
	@$(LIBC_ENV_rv) QEMU="$(QEMU_RV)" OUTDIR="$(CURDIR)/build/libc-rv" \
	 FS_INTERPOSE=1 FS_AMBIENT_DEFAULT=1 \
	 sh tests/run-libc-tests.sh

check-libc-mips: build/skj-cc-mips build/mips/start.o build/mips/half.o build/mips/va_mips.o build/mips/soft64_div.o build/mips/co_switch.o
	@$(LIBC_ENV_mips) QEMU="$(QEMU_MIPS)" OUTDIR="$(CURDIR)/build/libc-mips" \
	 sh tests/run-libc-tests.sh

check-libc-x86-64: build/skj-cc-x86-64 build/x86_64/start_sysv.o build/x86_64/half.o build/x86_64/va.o build/x86_64/soft64.o build/x86_64/co_switch.o
	@$(LIBC_ENV_x86_64) QEMU="$(QEMU_X64)" OUTDIR="$(CURDIR)/build/libc-x86_64" \
	 sh tests/run-libc-tests.sh

## The freestanding C libc running under the VMM: the same libc sources, built
## by skj-cc-x86-64 and linked with the HV crt and the doorbell syscall seam,
## run natively under KVM through skj-run.  The full phase set, including the
## file-I/O phases (statx, pathops, readdir): FS_INTERPOSE routes the libc fd
## model through the shared fs/ layer, and the ambient backend reaches the host
## by bare path (FS_AMBIENT_DEFAULT), the same way the rv and cf freestanding
## tiers run it.  This is VMM parity with the emulated sandboxes.  Skips cleanly
## without /dev/kvm.  The arm64 sibling has no host here to run on.
check-libc-hv-x86-64: $(VMM_PROBE_DEPS) build/skj-run build/skj-cc-x86-64 build/skj-as-x86-64 \
                      build/hv-x86_64/start.o build/hv-x86_64/syscall.o \
                      build/x86_64/half.o build/x86_64/va.o build/x86_64/soft64.o build/x86_64/co_switch.o
	@if ! $(vmm_works); then echo "SKIP: libc hv x86-64 (no functional KVM vcpu)"; exit 0; fi; \
	 CCBIN="$(CURDIR)/build/skj-cc-x86-64" \
	 ASM="$(CURDIR)/build/skj-as-x86-64 -o" \
	 LD="$(X64_LD) -m elf_x86_64 -e _start -Ttext=0x100000 --build-id=none -o" \
	 QEMU="$(CURDIR)/build/skj-run" \
	 START="$(CURDIR)/build/hv-x86_64/start.o" \
	 HALF="$(CURDIR)/build/hv-x86_64/syscall.o $(CURDIR)/build/x86_64/va.o $(CURDIR)/build/x86_64/half.o $(CURDIR)/build/x86_64/soft64.o $(CURDIR)/build/x86_64/co_switch.o" \
	 OUTDIR="$(CURDIR)/build/libc-hv-x86_64" \
	 FS_INTERPOSE=1 FS_AMBIENT_DEFAULT=1 \
	 sh tests/run-libc-tests.sh

## The aarch64 sibling: the same libc built by skj-cc-arm64 and linked with the
## arm64 HV crt and seam, the full set with FS_INTERPOSE.  It runs only on an
## aarch64 KVM host, so on any other host BUILD_ONLY verifies the
## compile/assemble/link path (full parity with the x86-64 tier, which runs here).
check-libc-hv-arm64: build/skj-cc-arm64 build/skj-as-aarch64 \
                     build/hv-arm64/start.o build/hv-arm64/syscall.o \
                     build/arm64/va.o build/arm64/soft64.o build/arm64/co_switch.o
	@if [ "$$(uname -m)" = "aarch64" ] && [ -e /dev/kvm ]; then B=; else B=1; fi; \
	 CCBIN="$(CURDIR)/build/skj-cc-arm64" \
	 ASM="$(CURDIR)/build/skj-as-aarch64 -o" \
	 LD="$(A64_LD) -e _start -Ttext=0x100000 --build-id=none -o" \
	 QEMU="$(CURDIR)/build/skj-run" \
	 START="$(CURDIR)/build/hv-arm64/start.o" \
	 HALF="$(CURDIR)/build/hv-arm64/syscall.o $(CURDIR)/build/arm64/va.o $(CURDIR)/build/arm64/soft64.o $(CURDIR)/build/arm64/co_switch.o" \
	 OUTDIR="$(CURDIR)/build/libc-hv-arm64" \
	 FS_INTERPOSE=1 FS_AMBIENT_DEFAULT=1 \
	 BUILD_ONLY="$$B" \
	 sh tests/run-libc-tests.sh

check-libc-arm64: build/skj-cc-arm64 build/arm64/start_aapcs.o build/arm64/va.o build/arm64/soft64.o build/arm64/co_switch.o
	@$(LIBC_ENV_arm64) QEMU="$(firstword $(QEMU_A64))" OUTDIR="$(CURDIR)/build/libc-arm64" \
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
	$(call run_cc_suite,$(CURDIR)/build/skj-cc-mips,$(MIPS_AS) -march=mips1 -EL -o,$(MIPS_LD) -o,$(QEMU_MIPS),$(CURDIR)/build/mips/start.o,$(CURDIR)/build/mips/half.o $(CURDIR)/build/mips/va_mips.o,$(CURDIR)/build/cc-mips,$(MIPS_EXCLUDE))

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
	$(call run_cc_suite,$(CURDIR)/build/skj-cc-mips,$(CURDIR)/build/skj-as-mips -o,$(CURDIR)/build/skj-ld-mips -o,$(QEMU_MIPS),$(CURDIR)/build/mips/start.o,$(CURDIR)/build/mips/half.o $(CURDIR)/build/mips/va_mips.o,$(CURDIR)/build/cc-mips-skj,$(MIPS_EXCLUDE))

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

## ---- libpsx: the PlayStation SDK (doc/tools/psx-sdk.md) ----
## Built soft-float (the R3051 has no FPU) with the binutils-free MIPS
## toolchain, archived into libpsx.a, and linked into a PS-EXE with the
## BIOS-TTY crt.  Milestone 1 is the GPU driver; later modules add to PSX_OBJS.
PSX_OBJS := build/mips/libpsx/gpu.o build/mips/libpsx/dma.o \
            build/mips/libpsx/irq.o build/mips/libpsx/exc.o \
            build/mips/libpsx/pad.o build/mips/libpsx/spu.o \
            build/mips/libpsx/mem.o build/mips/libpsx/fixed.o \
            build/mips/libpsx/trig_table.o build/mips/libpsx/pas_rt.o \
            build/mips/libpsx/kp_rt.o build/mips/libpsx/beep.o \
            build/mips/libpsx/fs.o build/mips/libpsx/romfs.o \
            build/mips/libpsx/isofs.o build/mips/libpsx/cd.o \
            build/mips/libpsx/tex.o build/mips/libpsx/gte.o \
            build/mips/libpsx/gte_mat.o build/mips/libpsx/timer.o \
            build/mips/libpsx/mc.o build/mips/libpsx/mc_fs.o \
            build/mips/libpsx/ot.o build/mips/libpsx/debug.o \
            build/mips/libpsx/gte_easy.o

## The built-in beep tone, generated by vagconv and compiled into libpsx so
## psx_beep needs no caller-supplied sample.
build/mips/libpsx/beep.c: build/skj-vagconv | build/mips/libpsx
	@./build/skj-vagconv --tone 523 250 22050 -c psx_beep_data $@

build/mips/libpsx/beep.o: build/mips/libpsx/beep.c build/skj-cc-mips-sf build/skj-as-mips | build/mips/libpsx
	@./build/skj-cc-mips-sf -o build/mips/libpsx/beep.s build/mips/libpsx/beep.c
	@./build/skj-as-mips -o $@ build/mips/libpsx/beep.s

## The sine table is generated by the host tool mktrig and compiled for the
## target; the same generated source is also compiled on the host for the
## fixed-point self-test (test-psx-fixed).
build/skj-mktrig: libpsx/tools/mktrig.c | build
	$(CC) $(CFLAGS) -o $@ libpsx/tools/mktrig.c -lm

build/psx/trig_table.c: build/skj-mktrig | build/psx
	@./build/skj-mktrig $@

build/mips/libpsx/trig_table.o: build/psx/trig_table.c build/skj-cc-mips-sf build/skj-as-mips | build/mips/libpsx
	@./build/skj-cc-mips-sf -o build/mips/libpsx/trig_table.s build/psx/trig_table.c
	@./build/skj-as-mips -o $@ build/mips/libpsx/trig_table.s

## The shared fs/ layer, reused verbatim for bare metal: the same fs/fs.c and
## fs/romfs.c the sandbox links, compiled with only FS_WITH_ROM so none of the
## syscall/launcher backends (FS_WITH_STREAMS/HOSTCAP/9P) are pulled in. The
## FS_WITH_* flags are the cut between the shared core and the host-specific
## backends; bare metal takes the core plus the romfs backend and nothing else.
build/mips/libpsx/fs.o: fs/fs.c fs/fs.h build/skj-cc-mips-sf build/skj-as-mips | build/mips/libpsx
	@./build/skj-cc-mips-sf -DFS_WITH_ROM -DFS_WITH_ISO -Ifs -o build/mips/libpsx/fs.s fs/fs.c
	@./build/skj-as-mips -o $@ build/mips/libpsx/fs.s

build/mips/libpsx/romfs.o: fs/romfs.c fs/romfs.h fs/fs.h build/skj-cc-mips-sf build/skj-as-mips | build/mips/libpsx
	@./build/skj-cc-mips-sf -DFS_WITH_ROM -Ifs -o build/mips/libpsx/romfs.s fs/romfs.c
	@./build/skj-as-mips -o $@ build/mips/libpsx/romfs.s

build/mips/libpsx/isofs.o: fs/isofs.c fs/isofs.h fs/fs.h fs/fs_internal.h build/skj-cc-mips-sf build/skj-as-mips | build/mips/libpsx
	@./build/skj-cc-mips-sf -DFS_WITH_ISO -Ifs -o build/mips/libpsx/isofs.s fs/isofs.c
	@./build/skj-as-mips -o $@ build/mips/libpsx/isofs.s

build/mips/libpsx/cd.o: libpsx/cd.c libpsx/psx.h fs/isofs.h fs/fs.h build/skj-cc-mips-sf build/skj-as-mips | build/mips/libpsx
	@./build/skj-cc-mips-sf -Ilibpsx -Ifs -o build/mips/libpsx/cd.s libpsx/cd.c
	@./build/skj-as-mips -o $@ build/mips/libpsx/cd.s

## vagconv: the host-side audio converter (PCM/WAV -> SPU-ADPCM C array).
build/skj-vagconv: libpsx/tools/vagconv.c | build
	$(CC) $(CFLAGS) -o $@ libpsx/tools/vagconv.c -lm

## shotcheck: the host-side framebuffer checker.  It decodes a capture written
## by libpsx/tools/redux_shot.lua (run under PCSX-Redux) and verifies pixels,
## the golden-framebuffer side of the libpsx test tier.
build/skj-shotcheck: libpsx/tools/shotcheck.c | build
	$(CC) $(CFLAGS) -o $@ libpsx/tools/shotcheck.c

## timconv: the host-side texture converter (PPM -> TIM, 8/16-bit).  mkppm
## writes the deterministic four-quadrant test pattern it converts.
build/skj-timconv: libpsx/tools/timconv.c | build
	$(CC) $(CFLAGS) -o $@ libpsx/tools/timconv.c -lm

build/skj-mkppm: libpsx/tools/mkppm.c | build
	$(CC) $(CFLAGS) -o $@ libpsx/tools/mkppm.c

## wavcheck: the host-side audio checker.  It measures the dominant frequency of
## the raw samples PCSX-Redux writes through SDL's disk audio driver, the
## golden-audio counterpart of shotcheck.
build/skj-wavcheck: libpsx/tools/wavcheck.c | build
	$(CC) $(CFLAGS) -o $@ libpsx/tools/wavcheck.c -lm

build/skj-mkcdda: libpsx/tools/mkcdda.c | build
	$(CC) $(CFLAGS) -o $@ libpsx/tools/mkcdda.c -lm

## The test-pattern texture, generated and converted to two TIMs (8-bit
## paletted and 16-bit direct) embedded as C arrays for tex_sprite.
build/psx/testpat.ppm: build/skj-mkppm | build/psx
	@./build/skj-mkppm 64 $@

build/psx/tex8.c: build/psx/testpat.ppm build/skj-timconv | build/psx
	@./build/skj-timconv build/psx/testpat.ppm $@ --8 --vram 320 0 --clut 0 480 -c psx_tex8

build/psx/tex16.c: build/psx/testpat.ppm build/skj-timconv | build/psx
	@./build/skj-timconv build/psx/testpat.ppm $@ --16 --vram 384 0 -c psx_tex16

build/psx/tex8.o: build/psx/tex8.c build/skj-cc-mips-sf build/skj-as-mips | build/psx
	@./build/skj-cc-mips-sf -o build/psx/tex8.s build/psx/tex8.c
	@./build/skj-as-mips -o $@ build/psx/tex8.s

build/psx/tex16.o: build/psx/tex16.c build/skj-cc-mips-sf build/skj-as-mips | build/psx
	@./build/skj-cc-mips-sf -o build/psx/tex16.s build/psx/tex16.c
	@./build/skj-as-mips -o $@ build/psx/tex16.s

## The rotation sample links the four-quadrant texture (for sprite-sheet cells)
## and a solid magenta texture (for the rotated sprite).
build/psx/solid.ppm: build/skj-mkppm | build/psx
	@./build/skj-mkppm 64 $@ --solid 255 0 255

build/psx/rtex.c: build/psx/testpat.ppm build/skj-timconv | build/psx
	@./build/skj-timconv build/psx/testpat.ppm $@ --8 --vram 320 0 --clut 0 480 -c psx_rtex

build/psx/stex.c: build/psx/solid.ppm build/skj-timconv | build/psx
	@./build/skj-timconv build/psx/solid.ppm $@ --8 --vram 384 0 --clut 0 481 -c psx_stex

build/psx/rtex.o: build/psx/rtex.c build/skj-cc-mips-sf build/skj-as-mips | build/psx
	@./build/skj-cc-mips-sf -o build/psx/rtex.s build/psx/rtex.c
	@./build/skj-as-mips -o $@ build/psx/rtex.s

build/psx/stex.o: build/psx/stex.c build/skj-cc-mips-sf build/skj-as-mips | build/psx
	@./build/skj-cc-mips-sf -o build/psx/stex.s build/psx/stex.c
	@./build/skj-as-mips -o $@ build/psx/stex.s

build/psx/tex_rot.exe: examples/psx/tex_rot.c libpsx/psx.h \
                 build/psx/rtex.o build/psx/stex.o \
                 build/mips/libpsx.a build/mips/start_psx.o \
                 build/skj-cc-mips-sf build/skj-as-mips build/skj-ld-mips | build/psx
	@./build/skj-cc-mips-sf -Ilibpsx -o build/psx/tex_rot.s $<
	@./build/skj-as-mips -o build/psx/tex_rot.o build/psx/tex_rot.s
	@./build/skj-ld-mips -f ps-exe -o $@ \
	    build/mips/start_psx.o build/psx/tex_rot.o \
	    build/psx/rtex.o build/psx/stex.o build/mips/libpsx.a

build/mips/libpsx/%.o: libpsx/%.c libpsx/psx.h build/skj-cc-mips-sf build/skj-as-mips | build/mips/libpsx
	@./build/skj-cc-mips-sf -Ilibpsx -o build/mips/libpsx/$*.s $<
	@./build/skj-as-mips -o $@ build/mips/libpsx/$*.s

build/mips/libpsx/%.o: libpsx/%.S build/skj-as-mips | build/mips/libpsx
	@./build/skj-as-mips -o $@ $<

build/mips/libpsx.a: $(PSX_OBJS) build/skj-ar | build/mips/libpsx
	@rm -f $@
	@./build/skj-ar rcs $@ $(PSX_OBJS)

## A libpsx sample: C -> skj-cc-mips-sf -> skj-as-mips -> skj-ld-mips, linked
## with the BIOS-TTY crt and libpsx.a into a PS-EXE the console loads.
build/psx/%.exe: examples/psx/%.c libpsx/psx.h build/mips/libpsx.a build/mips/start_psx.o \
                 build/skj-cc-mips-sf build/skj-as-mips build/skj-ld-mips | build/psx
	@./build/skj-cc-mips-sf -Ilibpsx -o build/psx/$*.s $<
	@./build/skj-as-mips -o build/psx/$*.o build/psx/$*.s
	@./build/skj-ld-mips -f ps-exe -o $@ \
	    build/mips/start_psx.o build/psx/$*.o build/mips/libpsx.a

## The milestone-2 sample links a generated SPU-ADPCM tone, so it has its own
## rule that adds tone.o to the link.
build/psx/tone.c: build/skj-vagconv | build/psx
	@./build/skj-vagconv --tone 440 1000 22050 -c psx_tone $@

build/psx/tone.o: build/psx/tone.c build/skj-cc-mips-sf build/skj-as-mips | build/psx
	@./build/skj-cc-mips-sf -o build/psx/tone.s build/psx/tone.c
	@./build/skj-as-mips -o $@ build/psx/tone.s

build/psx/input_sound.exe: examples/psx/input_sound.c libpsx/psx.h build/psx/tone.o \
                 build/mips/libpsx.a build/mips/start_psx.o \
                 build/skj-cc-mips-sf build/skj-as-mips build/skj-ld-mips | build/psx
	@./build/skj-cc-mips-sf -Ilibpsx -o build/psx/input_sound.s $<
	@./build/skj-as-mips -o build/psx/input_sound.o build/psx/input_sound.s
	@./build/skj-ld-mips -f ps-exe -o $@ \
	    build/mips/start_psx.o build/psx/input_sound.o build/psx/tone.o build/mips/libpsx.a

## The audio-capture sample plays a generated 440 Hz tone for the whole run.
build/psx/atone.c: build/skj-vagconv | build/psx
	@./build/skj-vagconv --tone 440 1000 22050 -c psx_atone $@

build/psx/atone.o: build/psx/atone.c build/skj-cc-mips-sf build/skj-as-mips | build/psx
	@./build/skj-cc-mips-sf -o build/psx/atone.s build/psx/atone.c
	@./build/skj-as-mips -o $@ build/psx/atone.s

build/psx/audio_tone.exe: examples/psx/audio_tone.c libpsx/psx.h build/psx/atone.o \
                 build/mips/libpsx.a build/mips/start_psx.o \
                 build/skj-cc-mips-sf build/skj-as-mips build/skj-ld-mips | build/psx
	@./build/skj-cc-mips-sf -Ilibpsx -o build/psx/audio_tone.s $<
	@./build/skj-as-mips -o build/psx/audio_tone.o build/psx/audio_tone.s
	@./build/skj-ld-mips -f ps-exe -o $@ \
	    build/mips/start_psx.o build/psx/audio_tone.o build/psx/atone.o build/mips/libpsx.a

## The ADSR-envelope and reverb sample reuses the 440 Hz capture tone.
build/psx/env_tone.exe: examples/psx/env_tone.c libpsx/psx.h build/psx/atone.o \
                 build/mips/libpsx.a build/mips/start_psx.o \
                 build/skj-cc-mips-sf build/skj-as-mips build/skj-ld-mips | build/psx
	@./build/skj-cc-mips-sf -Ilibpsx -o build/psx/env_tone.s $<
	@./build/skj-as-mips -o build/psx/env_tone.o build/psx/env_tone.s
	@./build/skj-ld-mips -f ps-exe -o $@ \
	    build/mips/start_psx.o build/psx/env_tone.o build/psx/atone.o build/mips/libpsx.a

## The texture sample links two generated TIM C arrays (8-bit and 16-bit).
build/psx/tex_sprite.exe: examples/psx/tex_sprite.c libpsx/psx.h \
                 build/psx/tex8.o build/psx/tex16.o \
                 build/mips/libpsx.a build/mips/start_psx.o \
                 build/skj-cc-mips-sf build/skj-as-mips build/skj-ld-mips | build/psx
	@./build/skj-cc-mips-sf -Ilibpsx -o build/psx/tex_sprite.s $<
	@./build/skj-as-mips -o build/psx/tex_sprite.o build/psx/tex_sprite.s
	@./build/skj-ld-mips -f ps-exe -o $@ \
	    build/mips/start_psx.o build/psx/tex_sprite.o \
	    build/psx/tex8.o build/psx/tex16.o build/mips/libpsx.a

## The milestone-3 asset sample: pack a seed directory into a romfs image,
## embed it as a C array, and link it beside the sample.  The shared fs/ layer
## (built into libpsx.a above) reads it at runtime through fs_open/fs_read.
build/psx/assets/sprite.raw: | build/psx
	@mkdir -p build/psx/assets
	@printf '\000\020\040\060\100\120\140\160\200\220\240\260\300\320\340\360' > $@

build/psx/asset.img: build/psx/assets/sprite.raw build/skj-mkromfs | build/psx
	@./build/skj-mkromfs build/psx/assets $@

build/psx/asset_img.c: build/psx/asset.img build/bin2c | build/psx
	@./build/bin2c psx_asset_img build/psx/asset.img build/psx/asset_img.c build/psx/psx_asset_img.h

build/psx/asset_img.o: build/psx/asset_img.c build/skj-cc-mips-sf build/skj-as-mips | build/psx
	@./build/skj-cc-mips-sf -Ibuild/psx -o build/psx/asset_img.s build/psx/asset_img.c
	@./build/skj-as-mips -o $@ build/psx/asset_img.s

build/psx/asset_rom.exe: examples/psx/asset_rom.c libpsx/psx.h build/psx/asset_img.o \
                 build/mips/libpsx.a build/mips/start_psx.o \
                 build/skj-cc-mips-sf build/skj-as-mips build/skj-ld-mips | build/psx
	@./build/skj-cc-mips-sf -Ilibpsx -Ifs -o build/psx/asset_rom.s $<
	@./build/skj-as-mips -o build/psx/asset_rom.o build/psx/asset_rom.s
	@./build/skj-ld-mips -f ps-exe -o $@ \
	    build/mips/start_psx.o build/psx/asset_rom.o build/psx/asset_img.o build/mips/libpsx.a

## The milestone-4 game sample links the romfs asset (a palette) and the tone.
build/psx/game.exe: examples/psx/game.c libpsx/psx.h build/psx/asset_img.o build/psx/tone.o \
                 build/mips/libpsx.a build/mips/start_psx.o \
                 build/skj-cc-mips-sf build/skj-as-mips build/skj-ld-mips | build/psx
	@./build/skj-cc-mips-sf -Ilibpsx -Ifs -o build/psx/game.s $<
	@./build/skj-as-mips -o build/psx/game.o build/psx/game.s
	@./build/skj-ld-mips -f ps-exe -o $@ \
	    build/mips/start_psx.o build/psx/game.o build/psx/asset_img.o build/psx/tone.o build/mips/libpsx.a

## Milestone 5: the Pascal front end reaching the PlayStation.  skj-pc-mips
## (soft-float) compiles a Pascal program that calls libpsx through externals;
## it links against start_psx and libpsx with the minimal Pascal runtime shim.
build/psx/hello_pas.exe: examples/psx/hello.pas build/skj-pc-mips build/mips/libpsx.a \
                 build/mips/start_psx.o build/skj-as-mips build/skj-ld-mips | build/psx
	@./build/skj-pc-mips -o build/psx/hello_pas.s examples/psx/hello.pas
	@./build/skj-as-mips -o build/psx/hello_pas.o build/psx/hello_pas.s
	@./build/skj-ld-mips -f ps-exe -o $@ \
	    build/mips/start_psx.o build/psx/hello_pas.o build/mips/libpsx.a

## The Kobold front end (skj-kpc-mips, emits a .target mips KPM) and AOT codegen
## (skj-kpc-mips, soft-float) reaching the PlayStation.  The kp_rt entry shim
## comes from libpsx.a.
build/psx/hello_kp.exe: examples/psx/hello.kp build/skj-kpc-mips build/skj-kpc-mips \
                 build/mips/libpsx.a build/mips/start_psx.o build/skj-as-mips build/skj-ld-mips | build/psx
	@./build/skj-kpc-mips --emit-kpm examples/psx/hello.kp > build/psx/hello_kp.kpm
	@./build/skj-kpc-mips build/psx/hello_kp.kpm -o build/psx/hello_kp.s
	@./build/skj-as-mips -o build/psx/hello_kp.o build/psx/hello_kp.s
	@./build/skj-ld-mips -f ps-exe -o $@ \
	    build/mips/start_psx.o build/psx/hello_kp.o build/mips/libpsx.a

## The Kobold inline-assembly sample: extended asm (write-only output, register
## inputs, an in-out operand) computes a rectangle's X position, drawn through
## the psx unit, so the framebuffer verifies the asm result.
build/psx/kasm.exe: examples/psx/kasm.kp build/psx-units/psx.kpm build/psx/psx_unit.o \
                 build/skj-kpc-mips build/skj-kpc-mips build/mips/libpsx.a \
                 build/mips/start_psx.o build/skj-as-mips build/skj-ld-mips | build/psx
	@./build/skj-kpc-mips -U build/psx-units --emit-kpm examples/psx/kasm.kp > build/psx/kasm.kpm
	@./build/skj-kpc-mips build/psx/kasm.kpm -o build/psx/kasm.s
	@./build/skj-as-mips -o build/psx/kasm.o build/psx/kasm.s
	@./build/skj-ld-mips -f ps-exe -o $@ \
	    build/mips/start_psx.o build/psx/kasm.o build/psx/psx_unit.o build/mips/libpsx.a

## Milestone 6: the thin language units and a game in each language.
## The Pascal game declares the psx bindings inline (Pascal has no unit
## system); the Kobold game uses the real `psx` unit, compiled to its own module
## and linked beside the program.
build/psx/game_pas.exe: examples/psx/game.pas build/skj-pc-mips build/mips/libpsx.a \
                 build/mips/start_psx.o build/skj-as-mips build/skj-ld-mips | build/psx
	@./build/skj-pc-mips -o build/psx/game_pas.s examples/psx/game.pas
	@./build/skj-as-mips -o build/psx/game_pas.o build/psx/game_pas.s
	@./build/skj-ld-mips -f ps-exe -o $@ \
	    build/mips/start_psx.o build/psx/game_pas.o build/mips/libpsx.a

## The Pascal 3D sample: a spinning cube through the GTE helpers and the ordering
## table, with the geometry and projection driven entirely from Pascal.
build/psx/cube_pas.exe: examples/psx/cube.pas build/skj-pc-mips build/mips/libpsx.a \
                 build/mips/start_psx.o build/skj-as-mips build/skj-ld-mips | build/psx
	@./build/skj-pc-mips -o build/psx/cube_pas.s examples/psx/cube.pas
	@./build/skj-as-mips -o build/psx/cube_pas.o build/psx/cube_pas.s
	@./build/skj-ld-mips -f ps-exe -o $@ \
	    build/mips/start_psx.o build/psx/cube_pas.o build/mips/libpsx.a

## The Pascal texture sample: the same richer draws through the inline bindings,
## with the textures loaded by the shared C accessor (sprite_asset.o).
build/psx/sprite_pas.exe: examples/psx/sprite.pas build/psx/sprite_asset.o \
                 build/psx/rtex.o build/psx/stex.o build/skj-pc-mips build/mips/libpsx.a \
                 build/mips/start_psx.o build/skj-as-mips build/skj-ld-mips | build/psx
	@./build/skj-pc-mips -o build/psx/sprite_pas.s examples/psx/sprite.pas
	@./build/skj-as-mips -o build/psx/sprite_pas.o build/psx/sprite_pas.s
	@./build/skj-ld-mips -f ps-exe -o $@ \
	    build/mips/start_psx.o build/psx/sprite_pas.o \
	    build/psx/sprite_asset.o build/psx/rtex.o build/psx/stex.o build/mips/libpsx.a

build/psx-units/psx.kpm: units/psx.kp build/skj-kpc-mips | build/psx-units
	@./build/skj-kpc-mips --emit-kpm units/psx.kp > $@

build/psx/psx_unit.o: build/psx-units/psx.kpm build/skj-kpc-mips build/skj-as-mips | build/psx
	@./build/skj-kpc-mips build/psx-units/psx.kpm -o build/psx/psx_unit.s
	@./build/skj-as-mips -o $@ build/psx/psx_unit.s

build/psx/game_kp.exe: examples/psx/game.kp build/psx-units/psx.kpm build/psx/psx_unit.o \
                 build/skj-kpc-mips build/skj-kpc-mips build/mips/libpsx.a \
                 build/mips/start_psx.o build/skj-as-mips build/skj-ld-mips | build/psx
	@./build/skj-kpc-mips -U build/psx-units --emit-kpm examples/psx/game.kp > build/psx/game_kp.kpm
	@./build/skj-kpc-mips build/psx/game_kp.kpm -o build/psx/game_kp.s
	@./build/skj-as-mips -o build/psx/game_kp.o build/psx/game_kp.s
	@./build/skj-ld-mips -f ps-exe -o $@ \
	    build/mips/start_psx.o build/psx/game_kp.o build/psx/psx_unit.o build/mips/libpsx.a

## The Kobold 3D sample: the same spinning cube through the psx unit's GTE
## helpers and ordering table.
build/psx/cube_kp.exe: examples/psx/cube.kp build/psx-units/psx.kpm build/psx/psx_unit.o \
                 build/skj-kpc-mips build/skj-kpc-mips build/mips/libpsx.a \
                 build/mips/start_psx.o build/skj-as-mips build/skj-ld-mips | build/psx
	@./build/skj-kpc-mips -U build/psx-units --emit-kpm examples/psx/cube.kp > build/psx/cube_kp.kpm
	@./build/skj-kpc-mips build/psx/cube_kp.kpm -o build/psx/cube_kp.s
	@./build/skj-as-mips -o build/psx/cube_kp.o build/psx/cube_kp.s
	@./build/skj-ld-mips -f ps-exe -o $@ \
	    build/mips/start_psx.o build/psx/cube_kp.o build/psx/psx_unit.o build/mips/libpsx.a

## The C accessor that loads the sprite samples' textures by handle (the pointer
## to the embedded TIM stays in C; Pascal and Kobold draw by the returned handle).
build/psx/sprite_asset.o: examples/psx/sprite_asset.c libpsx/psx.h \
                 build/skj-cc-mips-sf build/skj-as-mips | build/psx
	@./build/skj-cc-mips-sf -Ilibpsx -o build/psx/sprite_asset.s $<
	@./build/skj-as-mips -o $@ build/psx/sprite_asset.s

## The Kobold texture sample: drives the richer sprite draws through the psx unit.
build/psx/sprite_kp.exe: examples/psx/sprite.kp build/psx-units/psx.kpm build/psx/psx_unit.o \
                 build/psx/sprite_asset.o build/psx/rtex.o build/psx/stex.o \
                 build/skj-kpc-mips build/skj-kpc-mips build/mips/libpsx.a \
                 build/mips/start_psx.o build/skj-as-mips build/skj-ld-mips | build/psx
	@./build/skj-kpc-mips -U build/psx-units --emit-kpm examples/psx/sprite.kp > build/psx/sprite_kp.kpm
	@./build/skj-kpc-mips build/psx/sprite_kp.kpm -o build/psx/sprite_kp.s
	@./build/skj-as-mips -o build/psx/sprite_kp.o build/psx/sprite_kp.s
	@./build/skj-ld-mips -f ps-exe -o $@ \
	    build/mips/start_psx.o build/psx/sprite_kp.o build/psx/psx_unit.o \
	    build/psx/sprite_asset.o build/psx/rtex.o build/psx/stex.o build/mips/libpsx.a

## Milestone 7: ISO 9660 assets through the shared fs/ layer.  genisoimage packs
## a seed directory into an ISO image; isofs reads it.  The host test checks the
## reader against a real ISO; the embedded sample mounts the image from RAM and
## reads a file on the console.  genisoimage is an external tool, so these are
## opt-in (not in check-mips-all) and skip cleanly when it is absent.
ISOGEN := $(shell command -v genisoimage 2>/dev/null)

build/psx/isoseed/greet.txt: | build/psx
	@mkdir -p build/psx/isoseed
	@printf 'hello from the cd-rom' > $@
	@awk 'BEGIN{for(i=0;i<400;i++)printf("line %05d padding xyzzy\n", i)}' \
	    > build/psx/isoseed/big.txt

build/psx/test.iso: build/psx/isoseed/greet.txt | build/psx
	@genisoimage -quiet -o $@ -V PSXASSET build/psx/isoseed

## The CD-DA test disc: the data ISO as track 1 and a generated sine tone as a
## Red Book audio track 2, with a CUE that PCSX-Redux loads.  The CUE names the
## files by basename, so the .iso, .pcm, and .cue sit together in build/psx.
build/psx/cdda.cue: build/skj-mkcdda build/psx/test.iso | build/psx
	@cp build/psx/test.iso build/psx/cdda_data.iso
	@./build/skj-mkcdda --iso cdda_data.iso --freq 660 --secs 3 \
	    --out-pcm build/psx/cdda.pcm --out-cue $@

build/psx/iso_img.c: build/psx/test.iso build/bin2c | build/psx
	@./build/bin2c psx_iso_img build/psx/test.iso build/psx/iso_img.c build/psx/psx_iso_img.h

build/psx/iso_img.o: build/psx/iso_img.c build/skj-cc-mips-sf build/skj-as-mips | build/psx
	@./build/skj-cc-mips-sf -Ibuild/psx -o build/psx/iso_img.s build/psx/iso_img.c
	@./build/skj-as-mips -o $@ build/psx/iso_img.s

build/psx/asset_iso.exe: examples/psx/asset_iso.c libpsx/psx.h build/psx/iso_img.o \
                 build/mips/libpsx.a build/mips/start_psx.o \
                 build/skj-cc-mips-sf build/skj-as-mips build/skj-ld-mips | build/psx
	@./build/skj-cc-mips-sf -Ilibpsx -Ifs -o build/psx/asset_iso.s $<
	@./build/skj-as-mips -o build/psx/asset_iso.o build/psx/asset_iso.s
	@./build/skj-ld-mips -f ps-exe -o $@ \
	    build/mips/start_psx.o build/psx/asset_iso.o build/psx/iso_img.o build/mips/libpsx.a

## The CD-streaming sample reads the disc through the controller (no embedded
## image), so it needs the fs/ headers and a real disc under the emulator.
build/psx/cd_stream.exe: examples/psx/cd_stream.c libpsx/psx.h fs/fs.h \
                 build/mips/libpsx.a build/mips/start_psx.o \
                 build/skj-cc-mips-sf build/skj-as-mips build/skj-ld-mips | build/psx
	@./build/skj-cc-mips-sf -Ilibpsx -Ifs -o build/psx/cd_stream.s $<
	@./build/skj-as-mips -o build/psx/cd_stream.o build/psx/cd_stream.s
	@./build/skj-ld-mips -f ps-exe -o $@ \
	    build/mips/start_psx.o build/psx/cd_stream.o build/mips/libpsx.a

## Host test of the ISO 9660 reader (shared fs/ layer) against a real ISO.
build/psx/isotest: libpsx/tools/isotest.c fs/fs.c fs/isofs.c fs/fs.h fs/isofs.h fs/fs_internal.h | build/psx
	$(CC) $(CFLAGS) -Ifs -DFS_WITH_ISO -o $@ libpsx/tools/isotest.c fs/fs.c fs/isofs.c

test-psx-isofs: build/psx/isotest
	@if [ -z "$(ISOGEN)" ]; then echo "SKIP: genisoimage not found"; exit 0; fi; \
	 $(MAKE) build/psx/test.iso >/dev/null && ./build/psx/isotest build/psx/test.iso

test-psx-iso-redux:
	@if [ -z "$(ISOGEN)" ]; then echo "SKIP: genisoimage not found"; exit 0; fi; \
	 $(MAKE) build/psx/asset_iso.exe >/dev/null || exit 1; \
	 if ! command -v $(PCSX_REDUX) >/dev/null 2>&1; then echo "SKIP: PCSX-Redux not found"; exit 0; fi; \
	 if [ -z "$(PSX_BIOS)" ] || [ ! -f "$(PSX_BIOS)" ]; then echo "SKIP: no PlayStation BIOS"; exit 0; fi; \
	 env -u DISPLAY -u WAYLAND_DISPLAY timeout 60 $(PCSX_REDUX) -testmode -cli -run -no-gui-log \
	    -bios "$(PSX_BIOS)" -exe build/psx/asset_iso.exe >/dev/null 2>&1; \
	 rc=$$?; \
	 if [ $$rc -eq 0 ]; then echo "PASS: asset_iso (exit 0, ISO 9660 read via fs/ layer)"; \
	 else echo "FAIL: asset_iso (exit $$rc)"; exit 1; fi

## Functional CD-streaming test: boot the sample with a real data disc (-iso) so
## the ISO 9660 backend reads sectors on demand through the CD-ROM controller
## (libpsx/cd.c), not from an embedded RAM image.  The sample reads cd:greet.txt
## off the disc and draws a bar whose x comes from the file's first byte, so the
## framebuffer check confirms the read reached the program.  This is the
## functional verification of cd.c, which the bare PS-EXE path cannot give (it
## mounts no disc).  Opt-in: needs genisoimage, PCSX-Redux, and a BIOS.
test-psx-cd-redux: build/psx/cd_stream.exe build/skj-shotcheck
	@if [ -z "$(ISOGEN)" ]; then echo "SKIP: genisoimage not found"; exit 0; fi; \
	 $(MAKE) build/psx/test.iso >/dev/null || exit 1; \
	 if ! command -v $(PCSX_REDUX) >/dev/null 2>&1; then echo "SKIP: PCSX-Redux not found"; exit 0; fi; \
	 if [ -z "$(PSX_BIOS)" ] || [ ! -f "$(PSX_BIOS)" ]; then echo "SKIP: no PlayStation BIOS"; exit 0; fi; \
	 rm -f build/psx/cd.bin; \
	 env -u DISPLAY -u WAYLAND_DISPLAY \
	    PSX_SHOT="$(CURDIR)/build/psx/cd.bin" PSX_SHOT_FRAMES=40 \
	    timeout 60 $(PCSX_REDUX) -cli -softgpu -testmode -run -no-gui-log \
	    -bios "$(PSX_BIOS)" -iso build/psx/test.iso -exe build/psx/cd_stream.exe \
	    -dofile libpsx/tools/redux_shot.lua >/dev/null 2>&1; \
	 if [ ! -f build/psx/cd.bin ]; then echo "FAIL: cd_stream did not reach its draw loop (CD read failed)"; exit 1; fi; \
	 ./build/skj-shotcheck build/psx/cd.bin --ppm build/psx/cd.ppm --at 124 120 0 255 0 20; \
	 rc=$$?; \
	 if [ $$rc -eq 0 ]; then echo "PASS: cd_stream (disc read via cd.c, streamed through isofs)"; \
	 else echo "FAIL: cd_stream framebuffer (shotcheck $$rc)"; exit 1; fi

## Structural validation of the GPU sample's PS-EXE, the always-on tier (the
## functional run is test-mips-psexe-redux's model: a real BIOS on PCSX-Redux).
test-psx-gpu: build/psx/hello_gpu.exe
	@OBJDUMP="$(MIPS_OBJDUMP)" sh tests/run-psx-tests.sh $<

## Structural validation of the texture sample's PS-EXE, always-on.
test-psx-tex: build/psx/tex_sprite.exe
	@OBJDUMP="$(MIPS_OBJDUMP)" sh tests/run-psx-tests.sh $<

## Structural validation of the CD-streaming sample's PS-EXE, always-on (the
## functional disc-boot run is test-psx-cd-redux).
test-psx-cd: build/psx/cd_stream.exe
	@OBJDUMP="$(MIPS_OBJDUMP)" sh tests/run-psx-tests.sh $<

## Structural validation of the audio sample's PS-EXE, always-on (the functional
## sound-capture run is test-psx-audio-redux).
test-psx-audio: build/psx/audio_tone.exe
	@OBJDUMP="$(MIPS_OBJDUMP)" sh tests/run-psx-tests.sh $<

## Structural validation of the rotation/sprite-sheet sample, always-on (the
## functional framebuffer run is test-psx-rot-redux).
test-psx-rot: build/psx/tex_rot.exe
	@OBJDUMP="$(MIPS_OBJDUMP)" sh tests/run-psx-tests.sh $<

## Structural validation of the semi-transparency sample, always-on (the
## functional framebuffer run is test-psx-alpha-redux).
test-psx-alpha: build/psx/alpha.exe
	@OBJDUMP="$(MIPS_OBJDUMP)" sh tests/run-psx-tests.sh $<

## Structural validation of the GTE 3-D samples' PS-EXEs, always-on (the
## functional runs are test-psx-gte-redux and test-psx-cube-redux).
test-psx-gte: build/psx/gte_test.exe build/psx/gte3d.exe build/psx/cube.exe
	@OBJDUMP="$(MIPS_OBJDUMP)" sh tests/run-psx-tests.sh build/psx/gte_test.exe
	@OBJDUMP="$(MIPS_OBJDUMP)" sh tests/run-psx-tests.sh build/psx/gte3d.exe
	@OBJDUMP="$(MIPS_OBJDUMP)" sh tests/run-psx-tests.sh build/psx/cube.exe

## Structural validation of the root-counter sample, always-on (the functional
## run is test-psx-timer-redux).
test-psx-timer: build/psx/timer_test.exe
	@OBJDUMP="$(MIPS_OBJDUMP)" sh tests/run-psx-tests.sh $<

## Structural validation of the Gouraud-shading sample, always-on (the
## functional framebuffer run is test-psx-shade-redux).
test-psx-shade: build/psx/shade3d.exe
	@OBJDUMP="$(MIPS_OBJDUMP)" sh tests/run-psx-tests.sh $<

## Structural validation of the ADSR/reverb sample, always-on (the functional
## sound-capture run is test-psx-env-redux).
test-psx-env: build/psx/env_tone.exe
	@OBJDUMP="$(MIPS_OBJDUMP)" sh tests/run-psx-tests.sh $<

## Structural validation of the debug-text sample, always-on (the functional
## framebuffer run is test-psx-dbgtext-redux).
test-psx-dbgtext: build/psx/dbgtext.exe
	@OBJDUMP="$(MIPS_OBJDUMP)" sh tests/run-psx-tests.sh $<

## The host texture-quantizer self-test: median-cut a gradient and check the
## reconstruction error is small, and a low-color image stays exact.  Host-only.
test-psx-timconv: build/skj-timconv
	@./build/skj-timconv --selftest

## The host ADPCM encoder self-test: encode a tone, decode it, check the error
## is small.  Host-only, no emulator (the test-softfloat pattern).
test-psx-adpcm: build/skj-vagconv
	@./build/skj-vagconv --selftest

## The host fixed-point self-test: the target's fixed.c plus the generated trig
## table, compiled on the host and checked against double math.
build/psx/fixedtest: libpsx/tools/fixedtest.c libpsx/fixed.c build/psx/trig_table.c libpsx/psx.h | build/psx
	$(CC) $(CFLAGS) -Ilibpsx -o $@ libpsx/tools/fixedtest.c libpsx/fixed.c build/psx/trig_table.c -lm

test-psx-fixed: build/psx/fixedtest
	@./build/psx/fixedtest

## Milestone 3: assets from an embedded romfs through the shared fs/ layer.
test-psx-assets: build/psx/asset_rom.exe
	@OBJDUMP="$(MIPS_OBJDUMP)" sh tests/run-psx-tests.sh $<

## Milestone 4: the complete little game (fixed-point + input + sound + assets).
test-psx-game: build/psx/game.exe
	@OBJDUMP="$(MIPS_OBJDUMP)" sh tests/run-psx-tests.sh $<

## Milestone 5: the Pascal and Kobold front ends reaching the PlayStation.
## skj-pc-mips and skj-kpc-mips compile a sample in each language
## that calls libpsx through externals and links into a PS-EXE.
test-psx-lang: build/psx/hello_pas.exe build/psx/hello_kp.exe
	@OBJDUMP="$(MIPS_OBJDUMP)" sh tests/run-psx-tests.sh build/psx/hello_pas.exe
	@OBJDUMP="$(MIPS_OBJDUMP)" sh tests/run-psx-tests.sh build/psx/hello_kp.exe

test-psx-lang-redux: build/psx/hello_pas.exe build/psx/hello_kp.exe
	@if ! command -v $(PCSX_REDUX) >/dev/null 2>&1; then \
	    echo "SKIP: PCSX-Redux ($(PCSX_REDUX)) not found"; exit 0; fi; \
	 if [ -z "$(PSX_BIOS)" ] || [ ! -f "$(PSX_BIOS)" ]; then \
	    echo "SKIP: no PlayStation BIOS"; exit 0; fi; \
	 for e in build/psx/hello_pas.exe build/psx/hello_kp.exe; do \
	    env -u DISPLAY -u WAYLAND_DISPLAY timeout 60 $(PCSX_REDUX) -testmode -cli -run -no-gui-log \
	       -bios "$(PSX_BIOS)" -exe $$e >/dev/null 2>&1; \
	    rc=$$?; \
	    if [ $$rc -eq 0 ]; then echo "PASS: $$(basename $$e) (exit 0)"; \
	    else echo "FAIL: $$(basename $$e) (exit $$rc)"; exit 1; fi; \
	 done

## The texture/sprite API driven from Pascal and Kobold: the units now expose
## the handle-based sprite draws, so a managed-language program draws textures.
test-psx-sprite-lang: build/psx/sprite_pas.exe build/psx/sprite_kp.exe
	@OBJDUMP="$(MIPS_OBJDUMP)" sh tests/run-psx-tests.sh build/psx/sprite_pas.exe
	@OBJDUMP="$(MIPS_OBJDUMP)" sh tests/run-psx-tests.sh build/psx/sprite_kp.exe

## Framebuffer check that both language sprite samples draw the texture and the
## rotated sprite: the plain sprite's top-left cell is red and the rotated sprite
## reaches a pixel an axis-aligned one could not.  Opt-in like the others.
test-psx-sprite-lang-redux: build/psx/sprite_pas.exe build/psx/sprite_kp.exe build/skj-shotcheck | build/psx
	@if ! command -v $(PCSX_REDUX) >/dev/null 2>&1; then \
	    echo "SKIP: PCSX-Redux ($(PCSX_REDUX)) not found"; exit 0; fi; \
	 if [ -z "$(PSX_BIOS)" ] || [ ! -f "$(PSX_BIOS)" ]; then \
	    echo "SKIP: no PlayStation BIOS"; exit 0; fi; \
	 for e in build/psx/sprite_pas.exe build/psx/sprite_kp.exe; do \
	    b=$$(basename $$e .exe); \
	    rm -f build/psx/$$b.bin; \
	    env -u DISPLAY -u WAYLAND_DISPLAY \
	       PSX_SHOT="$(CURDIR)/build/psx/$$b.bin" PSX_SHOT_FRAMES=60 \
	       timeout 60 $(PCSX_REDUX) -cli -softgpu -testmode -run -no-gui-log \
	       -bios "$(PSX_BIOS)" -exe $$e \
	       -dofile libpsx/tools/redux_shot.lua >/dev/null 2>&1; \
	    if [ ! -f build/psx/$$b.bin ]; then echo "FAIL: $$b no framebuffer"; exit 1; fi; \
	    ./build/skj-shotcheck build/psx/$$b.bin \
	       --at 50 50 255 0 0 16 --at 238 120 255 0 255 20 >/dev/null; \
	    if [ $$? -eq 0 ]; then echo "PASS: $$b (texture + rotated sprite verified)"; \
	    else echo "FAIL: $$b framebuffer"; exit 1; fi; \
	 done

## The GTE/ordering-table 3D path driven from Pascal and Kobold: the units expose
## the pointer-free GTE helpers, so a managed-language program projects and draws
## a spinning cube.
test-psx-cube-lang: build/psx/cube_pas.exe build/psx/cube_kp.exe
	@OBJDUMP="$(MIPS_OBJDUMP)" sh tests/run-psx-tests.sh build/psx/cube_pas.exe
	@OBJDUMP="$(MIPS_OBJDUMP)" sh tests/run-psx-tests.sh build/psx/cube_kp.exe

## The skjcc `ps1` driver target: the whole C pipeline (skj-cc-mips-sf ->
## skj-as-mips -> skj-ld-mips -f ps-exe, linking start_psx.o and libpsx.a) in one
## command.  Stage the SDK where the in-tree driver looks (build/lib/ps1) and
## build the cube through it, matching the installed flow in doc/tools/psx-setup.md.
## One `skjcc -target ps1` command per language, exercising the driver's
## extension dispatch: C straight through, Pascal via skj-pc-mips, Kobold via
## the KPM front end + codegen, resolving `uses psx` from the staged unit dir
## and linking psx_unit.o.  The staged build/lib/ps1 mirrors the install layout.
test-psx-cube-driver: build/skjcc build/mips/libpsx.a build/mips/start_psx.o \
                      build/skj-cc-mips-sf build/skj-pc-mips build/skj-kpc-mips \
                      build/skj-kpc-mips build/skj-as-mips build/skj-ld-mips \
                      build/psx-units/psx.kpm build/psx/psx_unit.o | build/psx
	@mkdir -p build/lib/ps1
	@cp build/mips/libpsx.a build/mips/start_psx.o libpsx/psx.h build/lib/ps1/
	@cp build/psx-units/psx.kpm build/psx/psx_unit.o build/lib/ps1/
	@./build/skjcc -target ps1 -o build/psx/cube_drv.exe examples/psx/cube.c
	@./build/skjcc -target ps1 -o build/psx/cube_pas_drv.exe examples/psx/cube.pas
	@./build/skjcc -target ps1 -o build/psx/cube_kp_drv.exe examples/psx/cube.kp
	@OBJDUMP="$(MIPS_OBJDUMP)" sh tests/run-psx-tests.sh build/psx/cube_drv.exe
	@OBJDUMP="$(MIPS_OBJDUMP)" sh tests/run-psx-tests.sh build/psx/cube_pas_drv.exe
	@OBJDUMP="$(MIPS_OBJDUMP)" sh tests/run-psx-tests.sh build/psx/cube_kp_drv.exe

## Framebuffer check that both language cube samples project and draw 3D faces:
## at frame 40 the cube's centre shows a face colour (green with this compose
## order) while a corner keeps the background, so the GTE projection and the
## ordering table are verified end to end from each language.  Opt-in.
test-psx-cube-lang-redux: build/psx/cube_pas.exe build/psx/cube_kp.exe build/skj-shotcheck | build/psx
	@if ! command -v $(PCSX_REDUX) >/dev/null 2>&1; then \
	    echo "SKIP: PCSX-Redux ($(PCSX_REDUX)) not found"; exit 0; fi; \
	 if [ -z "$(PSX_BIOS)" ] || [ ! -f "$(PSX_BIOS)" ]; then \
	    echo "SKIP: no PlayStation BIOS"; exit 0; fi; \
	 for e in build/psx/cube_pas.exe build/psx/cube_kp.exe; do \
	    b=$$(basename $$e .exe); \
	    rm -f build/psx/$$b.bin; \
	    env -u DISPLAY -u WAYLAND_DISPLAY \
	       PSX_SHOT="$(CURDIR)/build/psx/$$b.bin" PSX_SHOT_FRAMES=40 \
	       timeout 60 $(PCSX_REDUX) -cli -softgpu -testmode -run -no-gui-log \
	       -bios "$(PSX_BIOS)" -exe $$e \
	       -dofile libpsx/tools/redux_shot.lua >/dev/null 2>&1; \
	    if [ ! -f build/psx/$$b.bin ]; then echo "FAIL: $$b no framebuffer"; exit 1; fi; \
	    ./build/skj-shotcheck build/psx/$$b.bin \
	       --at 160 120 0 255 0 60 --at 8 8 0 0 16 24 >/dev/null; \
	    if [ $$? -eq 0 ]; then echo "PASS: $$b (3D cube face at centre, background intact)"; \
	    else echo "FAIL: $$b framebuffer"; exit 1; fi; \
	 done

## Kobold inline assembly: structural gate (always-on), the functional run is
## test-psx-kasm-redux.
test-psx-kasm: build/psx/kasm.exe
	@OBJDUMP="$(MIPS_OBJDUMP)" sh tests/run-psx-tests.sh $<

## Kobold inline-asm framebuffer tier: the sample computes a rectangle's X with
## extended inline asm (output, inputs, in-out operand) and draws it through the
## psx unit.  The capture probes the rectangle's centre for white and a corner
## for the background, so a correct asm result lands the rectangle where probed
## and a wrong or ignored one does not.  Opt-in.
test-psx-kasm-redux: build/psx/kasm.exe build/skj-shotcheck | build/psx
	@if ! command -v $(PCSX_REDUX) >/dev/null 2>&1; then \
	    echo "SKIP: PCSX-Redux ($(PCSX_REDUX)) not found"; exit 0; fi; \
	 if [ -z "$(PSX_BIOS)" ] || [ ! -f "$(PSX_BIOS)" ]; then \
	    echo "SKIP: no PlayStation BIOS"; exit 0; fi; \
	 rm -f build/psx/kasm.bin; \
	 env -u DISPLAY -u WAYLAND_DISPLAY \
	    PSX_SHOT="$(CURDIR)/build/psx/kasm.bin" PSX_SHOT_FRAMES=30 \
	    timeout 60 $(PCSX_REDUX) -cli -softgpu -testmode -run -no-gui-log \
	    -bios "$(PSX_BIOS)" -exe $< \
	    -dofile libpsx/tools/redux_shot.lua >/dev/null 2>&1; \
	 if [ ! -f build/psx/kasm.bin ]; then echo "FAIL: kasm no framebuffer"; exit 1; fi; \
	 ./build/skj-shotcheck build/psx/kasm.bin \
	    --at 62 120 255 255 255 16 --at 8 8 0 0 16 24 >/dev/null; \
	 if [ $$? -eq 0 ]; then echo "PASS: kasm (inline-asm-placed rectangle verified)"; \
	 else echo "FAIL: kasm framebuffer (asm result wrong)"; exit 1; fi

## Milestone 6: the games built through the units, in Pascal and Kobold.
test-psx-units: build/psx/game_pas.exe build/psx/game_kp.exe
	@OBJDUMP="$(MIPS_OBJDUMP)" sh tests/run-psx-tests.sh build/psx/game_pas.exe
	@OBJDUMP="$(MIPS_OBJDUMP)" sh tests/run-psx-tests.sh build/psx/game_kp.exe

test-psx-units-redux: build/psx/game_pas.exe build/psx/game_kp.exe
	@if ! command -v $(PCSX_REDUX) >/dev/null 2>&1; then \
	    echo "SKIP: PCSX-Redux ($(PCSX_REDUX)) not found"; exit 0; fi; \
	 if [ -z "$(PSX_BIOS)" ] || [ ! -f "$(PSX_BIOS)" ]; then \
	    echo "SKIP: no PlayStation BIOS"; exit 0; fi; \
	 for e in build/psx/game_pas.exe build/psx/game_kp.exe; do \
	    env -u DISPLAY -u WAYLAND_DISPLAY timeout 90 $(PCSX_REDUX) -testmode -cli -run -no-gui-log \
	       -bios "$(PSX_BIOS)" -exe $$e >/dev/null 2>&1; \
	    rc=$$?; \
	    if [ $$rc -eq 0 ]; then echo "PASS: $$(basename $$e) (exit 0)"; \
	    else echo "FAIL: $$(basename $$e) (exit $$rc)"; exit 1; fi; \
	 done

test-psx-game-redux: build/psx/game.exe
	@if ! command -v $(PCSX_REDUX) >/dev/null 2>&1; then \
	    echo "SKIP: PCSX-Redux ($(PCSX_REDUX)) not found"; exit 0; fi; \
	 if [ -z "$(PSX_BIOS)" ] || [ ! -f "$(PSX_BIOS)" ]; then \
	    echo "SKIP: no PlayStation BIOS"; exit 0; fi; \
	 env -u DISPLAY -u WAYLAND_DISPLAY timeout 90 $(PCSX_REDUX) -testmode -cli -run -no-gui-log \
	    -bios "$(PSX_BIOS)" -exe $< >/dev/null 2>&1; \
	 rc=$$?; \
	 if [ $$rc -eq 0 ]; then echo "PASS: game (exit 0, full C SDK demo)"; \
	 else echo "FAIL: game (exit $$rc)"; exit 1; fi

test-psx-assets-redux: build/psx/asset_rom.exe
	@if ! command -v $(PCSX_REDUX) >/dev/null 2>&1; then \
	    echo "SKIP: PCSX-Redux ($(PCSX_REDUX)) not found"; exit 0; fi; \
	 if [ -z "$(PSX_BIOS)" ] || [ ! -f "$(PSX_BIOS)" ]; then \
	    echo "SKIP: no PlayStation BIOS"; exit 0; fi; \
	 env -u DISPLAY -u WAYLAND_DISPLAY timeout 60 $(PCSX_REDUX) -testmode -cli -run -no-gui-log \
	    -bios "$(PSX_BIOS)" -exe $< >/dev/null 2>&1; \
	 rc=$$?; \
	 if [ $$rc -eq 0 ]; then echo "PASS: asset_rom (exit 0, romfs read + fs/ layer)"; \
	 else echo "FAIL: asset_rom (exit $$rc)"; exit 1; fi

## Structural validation of the milestone-2 samples (pad probe, input+sound).
test-psx-m2: build/psx/pad_probe.exe build/psx/input_sound.exe
	@OBJDUMP="$(MIPS_OBJDUMP)" sh tests/run-psx-tests.sh build/psx/pad_probe.exe
	@OBJDUMP="$(MIPS_OBJDUMP)" sh tests/run-psx-tests.sh build/psx/input_sound.exe

## Functional run of the milestone-2 samples on PCSX-Redux with a real BIOS.
## pad_probe exits 0 when a digital pad answers idle; input_sound exits 0 after
## its loop, exercising the GPU/DMA/vsync/pad/SPU paths end to end.  Opt-in.
test-psx-m2-redux: build/psx/pad_probe.exe build/psx/input_sound.exe
	@if ! command -v $(PCSX_REDUX) >/dev/null 2>&1; then \
	    echo "SKIP: PCSX-Redux ($(PCSX_REDUX)) not found"; exit 0; fi; \
	 if [ -z "$(PSX_BIOS)" ] || [ ! -f "$(PSX_BIOS)" ]; then \
	    echo "SKIP: no PlayStation BIOS"; exit 0; fi; \
	 for e in build/psx/pad_probe.exe build/psx/input_sound.exe; do \
	    env -u DISPLAY -u WAYLAND_DISPLAY timeout 90 $(PCSX_REDUX) -testmode -cli -run -no-gui-log \
	       -bios "$(PSX_BIOS)" -exe $$e >/dev/null 2>&1; \
	    rc=$$?; \
	    if [ $$rc -eq 0 ]; then echo "PASS: $$(basename $$e) (exit 0)"; \
	    else echo "FAIL: $$(basename $$e) (exit $$rc)"; exit 1; fi; \
	 done

## Functional run of the GPU sample on PCSX-Redux with a real BIOS.  The sample
## exits 0 after its animation, so a clean exit inside the timeout proves the
## DMA transfer and the interrupt-driven vsync run end to end: a hung vsync (a
## broken IRQ handler) would time out and fail.  Opt-in like test-mips-psexe-redux.
test-psx-gpu-redux: build/psx/hello_gpu.exe
	@if ! command -v $(PCSX_REDUX) >/dev/null 2>&1; then \
	    echo "SKIP: PCSX-Redux ($(PCSX_REDUX)) not found"; exit 0; fi; \
	 if [ -z "$(PSX_BIOS)" ] || [ ! -f "$(PSX_BIOS)" ]; then \
	    echo "SKIP: no PlayStation BIOS (pass PSX_BIOS=/path/to/scphXXXX.bin)"; exit 0; fi; \
	 env -u DISPLAY -u WAYLAND_DISPLAY timeout 60 $(PCSX_REDUX) -testmode -cli -run -no-gui-log \
	    -bios "$(PSX_BIOS)" -exe $< >/dev/null 2>&1; \
	 rc=$$?; \
	 if [ $$rc -eq 0 ]; then echo "PASS: hello_gpu (exit 0, ran to completion)"; \
	 else echo "FAIL: hello_gpu (exit $$rc)"; exit 1; fi

## Golden-framebuffer tier: capture what the GPU actually puts on screen and
## check the pixels, not just that the sample exits.  redux_shot.lua drives
## PCSX-Redux through its Lua API to grab the displayed framebuffer after 90
## frames and write it to a file; it opens no window (-cli -softgpu renders in
## software with no OpenGL context), so it runs anywhere the exit-code tier
## does.  shotcheck then decodes the capture and verifies the dark-blue field
## and the yellow square hello_gpu draws.  DISPLAY is cleared so a stray window
## can never reach a real display.  Opt-in like the other redux tests.
test-psx-shot-redux: build/psx/hello_gpu.exe build/skj-shotcheck | build/psx
	@if ! command -v $(PCSX_REDUX) >/dev/null 2>&1; then \
	    echo "SKIP: PCSX-Redux ($(PCSX_REDUX)) not found"; exit 0; fi; \
	 if [ -z "$(PSX_BIOS)" ] || [ ! -f "$(PSX_BIOS)" ]; then \
	    echo "SKIP: no PlayStation BIOS (pass PSX_BIOS=/path/to/scphXXXX.bin)"; exit 0; fi; \
	 rm -f build/psx/shot.bin; \
	 env -u DISPLAY -u WAYLAND_DISPLAY \
	    PSX_SHOT="$(CURDIR)/build/psx/shot.bin" PSX_SHOT_FRAMES=90 \
	    timeout 60 $(PCSX_REDUX) -cli -softgpu -testmode -run -no-gui-log \
	    -bios "$(PSX_BIOS)" -exe $< \
	    -dofile libpsx/tools/redux_shot.lua >/dev/null 2>&1; \
	 if [ ! -f build/psx/shot.bin ]; then echo "FAIL: no framebuffer captured"; exit 1; fi; \
	 ./build/skj-shotcheck build/psx/shot.bin --ppm build/psx/shot.ppm \
	    --at 4 4 0 0 41 12 --find 255 206 0 16; \
	 rc=$$?; \
	 if [ $$rc -eq 0 ]; then echo "PASS: hello_gpu framebuffer (field + square verified)"; \
	 else echo "FAIL: hello_gpu framebuffer (shotcheck $$rc)"; exit 1; fi

## Framebuffer check of the texture pipeline: capture tex_sprite's frame and
## probe the four quadrant colors of both the 8-bit and the 16-bit sprite.  This
## verifies the TIM upload, the palette, the texture page, and the textured-rect
## primitive all put the right texels on screen, not just a clean exit.  The
## 8-bit sprite is at screen (48,40), the 16-bit at (208,40); each is 64x64 with
## red/green/blue/white quadrants.  Opt-in like the other redux tests.
test-psx-tex-redux: build/psx/tex_sprite.exe build/skj-shotcheck | build/psx
	@if ! command -v $(PCSX_REDUX) >/dev/null 2>&1; then \
	    echo "SKIP: PCSX-Redux ($(PCSX_REDUX)) not found"; exit 0; fi; \
	 if [ -z "$(PSX_BIOS)" ] || [ ! -f "$(PSX_BIOS)" ]; then \
	    echo "SKIP: no PlayStation BIOS (pass PSX_BIOS=/path/to/scphXXXX.bin)"; exit 0; fi; \
	 rm -f build/psx/tex.bin; \
	 env -u DISPLAY -u WAYLAND_DISPLAY \
	    PSX_SHOT="$(CURDIR)/build/psx/tex.bin" PSX_SHOT_FRAMES=90 \
	    timeout 60 $(PCSX_REDUX) -cli -softgpu -testmode -run -no-gui-log \
	    -bios "$(PSX_BIOS)" -exe $< \
	    -dofile libpsx/tools/redux_shot.lua >/dev/null 2>&1; \
	 if [ ! -f build/psx/tex.bin ]; then echo "FAIL: no framebuffer captured"; exit 1; fi; \
	 ./build/skj-shotcheck build/psx/tex.bin --ppm build/psx/tex.ppm \
	    --at 64 56 255 0 0 16 --at 96 56 0 255 0 16 \
	    --at 64 88 0 0 255 16 --at 96 88 255 255 255 16 \
	    --at 224 56 255 0 0 16 --at 256 56 0 255 0 16 \
	    --at 224 88 0 0 255 16 --at 256 88 255 255 255 16; \
	 rc=$$?; \
	 if [ $$rc -eq 0 ]; then echo "PASS: tex_sprite framebuffer (8-bit + 16-bit sprites verified)"; \
	 else echo "FAIL: tex_sprite framebuffer (shotcheck $$rc)"; exit 1; fi

## Framebuffer check of the richer sprite draws: two sprite-sheet cells (red and
## green) and a solid sprite rotated 45 degrees.  The rotation is confirmed by a
## probe at (238,120), which is outside the reach of an axis-aligned sprite of
## the same size, so only a real rotation colors it.  Opt-in like the others.
test-psx-rot-redux: build/psx/tex_rot.exe build/skj-shotcheck | build/psx
	@if ! command -v $(PCSX_REDUX) >/dev/null 2>&1; then \
	    echo "SKIP: PCSX-Redux ($(PCSX_REDUX)) not found"; exit 0; fi; \
	 if [ -z "$(PSX_BIOS)" ] || [ ! -f "$(PSX_BIOS)" ]; then \
	    echo "SKIP: no PlayStation BIOS (pass PSX_BIOS=/path/to/scphXXXX.bin)"; exit 0; fi; \
	 rm -f build/psx/rot.bin; \
	 env -u DISPLAY -u WAYLAND_DISPLAY \
	    PSX_SHOT="$(CURDIR)/build/psx/rot.bin" PSX_SHOT_FRAMES=60 \
	    timeout 60 $(PCSX_REDUX) -cli -softgpu -testmode -run -no-gui-log \
	    -bios "$(PSX_BIOS)" -exe $< \
	    -dofile libpsx/tools/redux_shot.lua >/dev/null 2>&1; \
	 if [ ! -f build/psx/rot.bin ]; then echo "FAIL: no framebuffer captured"; exit 1; fi; \
	 ./build/skj-shotcheck build/psx/rot.bin --ppm build/psx/rot.ppm \
	    --at 56 56 255 0 0 16 --at 106 56 0 255 0 16 \
	    --at 200 120 255 0 255 20 --at 238 120 255 0 255 20; \
	 rc=$$?; \
	 if [ $$rc -eq 0 ]; then echo "PASS: tex_rot (sprite-sheet cells + rotated sprite verified)"; \
	 else echo "FAIL: tex_rot framebuffer (shotcheck $$rc)"; exit 1; fi

## Framebuffer check of semi-transparency: an opaque red rect and a half blue
## rect over it.  The overlap must read as the blend of the two, the red-only
## region as opaque red.  Opt-in like the others.
test-psx-alpha-redux: build/psx/alpha.exe build/skj-shotcheck | build/psx
	@if ! command -v $(PCSX_REDUX) >/dev/null 2>&1; then \
	    echo "SKIP: PCSX-Redux ($(PCSX_REDUX)) not found"; exit 0; fi; \
	 if [ -z "$(PSX_BIOS)" ] || [ ! -f "$(PSX_BIOS)" ]; then \
	    echo "SKIP: no PlayStation BIOS (pass PSX_BIOS=/path/to/scphXXXX.bin)"; exit 0; fi; \
	 rm -f build/psx/alpha.bin; \
	 env -u DISPLAY -u WAYLAND_DISPLAY \
	    PSX_SHOT="$(CURDIR)/build/psx/alpha.bin" PSX_SHOT_FRAMES=60 \
	    timeout 60 $(PCSX_REDUX) -cli -softgpu -testmode -run -no-gui-log \
	    -bios "$(PSX_BIOS)" -exe $< \
	    -dofile libpsx/tools/redux_shot.lua >/dev/null 2>&1; \
	 if [ ! -f build/psx/alpha.bin ]; then echo "FAIL: no framebuffer captured"; exit 1; fi; \
	 ./build/skj-shotcheck build/psx/alpha.bin --ppm build/psx/alpha.ppm \
	    --at 115 130 255 0 0 16 --at 145 130 123 0 123 24; \
	 rc=$$?; \
	 if [ $$rc -eq 0 ]; then echo "PASS: alpha (opaque red + blended overlap verified)"; \
	 else echo "FAIL: alpha framebuffer (shotcheck $$rc)"; exit 1; fi

## GTE exit-code tier: gte_test projects known vertices through the real
## coprocessor 2 and exits 0 only if every screen position matches the
## perspective arithmetic, the way the FPU/i64 self-tests check arithmetic.
## This is the functional verification of the GTE path itself (set_rot/trans/
## screen and rtps); the structural tier only validates the PS-EXE.  Opt-in.
test-psx-gte-redux: build/psx/gte_test.exe build/psx/gte3d.exe
	@if ! command -v $(PCSX_REDUX) >/dev/null 2>&1; then \
	    echo "SKIP: PCSX-Redux ($(PCSX_REDUX)) not found"; exit 0; fi; \
	 if [ -z "$(PSX_BIOS)" ] || [ ! -f "$(PSX_BIOS)" ]; then \
	    echo "SKIP: no PlayStation BIOS (pass PSX_BIOS=/path/to/scphXXXX.bin)"; exit 0; fi; \
	 env -u DISPLAY -u WAYLAND_DISPLAY timeout 60 $(PCSX_REDUX) -testmode -cli -run -no-gui-log \
	    -bios "$(PSX_BIOS)" -exe build/psx/gte_test.exe >/dev/null 2>&1; \
	 rc=$$?; \
	 if [ $$rc -ne 0 ]; then echo "FAIL: gte_test (exit $$rc names the wrong vertex)"; exit 1; fi; \
	 echo "PASS: gte_test (RTPS projection verified on the GTE)"; \
	 env -u DISPLAY -u WAYLAND_DISPLAY timeout 60 $(PCSX_REDUX) -testmode -cli -run -no-gui-log \
	    -bios "$(PSX_BIOS)" -exe build/psx/gte3d.exe >/dev/null 2>&1; \
	 rc=$$?; \
	 if [ $$rc -eq 0 ]; then echo "PASS: gte3d (RTPT, NCLIP, AVSZ3, X/Z matrices, compose verified)"; \
	 else echo "FAIL: gte3d (exit $$rc names the failed check)"; exit 1; fi

## Framebuffer check of the spinning cube: the GTE projects all eight vertices
## each frame and the faces are drawn far-to-near, so at a fixed frame the near
## face covers the screen center.  A blank center means the 3-D projection or
## the depth-sorted fill did not reach the display.  Opt-in like the others.
test-psx-cube-redux: build/psx/cube.exe build/skj-shotcheck | build/psx
	@if ! command -v $(PCSX_REDUX) >/dev/null 2>&1; then \
	    echo "SKIP: PCSX-Redux ($(PCSX_REDUX)) not found"; exit 0; fi; \
	 if [ -z "$(PSX_BIOS)" ] || [ ! -f "$(PSX_BIOS)" ]; then \
	    echo "SKIP: no PlayStation BIOS (pass PSX_BIOS=/path/to/scphXXXX.bin)"; exit 0; fi; \
	 rm -f build/psx/cube.bin; \
	 env -u DISPLAY -u WAYLAND_DISPLAY \
	    PSX_SHOT="$(CURDIR)/build/psx/cube.bin" PSX_SHOT_FRAMES=40 \
	    timeout 60 $(PCSX_REDUX) -cli -softgpu -testmode -run -no-gui-log \
	    -bios "$(PSX_BIOS)" -exe $< \
	    -dofile libpsx/tools/redux_shot.lua >/dev/null 2>&1; \
	 if [ ! -f build/psx/cube.bin ]; then echo "FAIL: no framebuffer captured"; exit 1; fi; \
	 ./build/skj-shotcheck build/psx/cube.bin --ppm build/psx/cube.ppm \
	    --at 160 120 255 0 0 40; \
	 rc=$$?; \
	 if [ $$rc -eq 0 ]; then echo "PASS: cube (GTE-projected face covers the screen center)"; \
	 else echo "FAIL: cube framebuffer (shotcheck $$rc)"; exit 1; fi

## Root-counter exit-code tier: timer_test checks counter 1 counts one frame of
## horizontal blanks (~263, a hardware-exact rate) and the system clock runs far
## faster, so it verifies the counters tick at the right rate on real timing
## hardware, not just that they move.  Opt-in.
test-psx-timer-redux: build/psx/timer_test.exe
	@if ! command -v $(PCSX_REDUX) >/dev/null 2>&1; then \
	    echo "SKIP: PCSX-Redux ($(PCSX_REDUX)) not found"; exit 0; fi; \
	 if [ -z "$(PSX_BIOS)" ] || [ ! -f "$(PSX_BIOS)" ]; then \
	    echo "SKIP: no PlayStation BIOS (pass PSX_BIOS=/path/to/scphXXXX.bin)"; exit 0; fi; \
	 env -u DISPLAY -u WAYLAND_DISPLAY timeout 60 $(PCSX_REDUX) -testmode -cli -run -no-gui-log \
	    -bios "$(PSX_BIOS)" -exe $< >/dev/null 2>&1; \
	 rc=$$?; \
	 if [ $$rc -eq 0 ]; then echo "PASS: timer_test (root counters tick at the right rate)"; \
	 else echo "FAIL: timer_test (exit $$rc names the failed check)"; exit 1; fi

## Framebuffer check of Gouraud shading: one big quad with red, green, blue, and
## white corners.  The GPU interpolates, so the framebuffer corners read close to
## the corner colors.  This also confirms a primitive drawn through the ordering
## table reaches the screen.  Opt-in like the others.
test-psx-shade-redux: build/psx/shade3d.exe build/skj-shotcheck | build/psx
	@if ! command -v $(PCSX_REDUX) >/dev/null 2>&1; then \
	    echo "SKIP: PCSX-Redux ($(PCSX_REDUX)) not found"; exit 0; fi; \
	 if [ -z "$(PSX_BIOS)" ] || [ ! -f "$(PSX_BIOS)" ]; then \
	    echo "SKIP: no PlayStation BIOS (pass PSX_BIOS=/path/to/scphXXXX.bin)"; exit 0; fi; \
	 rm -f build/psx/shade.bin; \
	 env -u DISPLAY -u WAYLAND_DISPLAY \
	    PSX_SHOT="$(CURDIR)/build/psx/shade.bin" PSX_SHOT_FRAMES=30 \
	    timeout 60 $(PCSX_REDUX) -cli -softgpu -testmode -run -no-gui-log \
	    -bios "$(PSX_BIOS)" -exe $< \
	    -dofile libpsx/tools/redux_shot.lua >/dev/null 2>&1; \
	 if [ ! -f build/psx/shade.bin ]; then echo "FAIL: no framebuffer captured"; exit 1; fi; \
	 ./build/skj-shotcheck build/psx/shade.bin --ppm build/psx/shade.ppm \
	    --at 44 44 255 0 0 60 --at 276 44 0 255 0 60 \
	    --at 44 196 0 0 255 60 --at 276 196 255 255 255 60; \
	 rc=$$?; \
	 if [ $$rc -eq 0 ]; then echo "PASS: shade3d (Gouraud corners red/green/blue/white)"; \
	 else echo "FAIL: shade3d framebuffer (shotcheck $$rc)"; exit 1; fi

## Debug-text framebuffer tier: print white text on a dark background and check
## that white glyph pixels appear (the font drew) while a corner away from the
## text keeps the background colour (the text did not flood the screen).  This
## verifies the font upload and the per-character textured-rect draw.  DISPLAY is
## cleared.  Opt-in.
test-psx-dbgtext-redux: build/psx/dbgtext.exe build/skj-shotcheck | build/psx
	@if ! command -v $(PCSX_REDUX) >/dev/null 2>&1; then \
	    echo "SKIP: PCSX-Redux ($(PCSX_REDUX)) not found"; exit 0; fi; \
	 if [ -z "$(PSX_BIOS)" ] || [ ! -f "$(PSX_BIOS)" ]; then \
	    echo "SKIP: no PlayStation BIOS (pass PSX_BIOS=/path/to/scphXXXX.bin)"; exit 0; fi; \
	 rm -f build/psx/dbgtext.bin; \
	 env -u DISPLAY -u WAYLAND_DISPLAY \
	    PSX_SHOT="$(CURDIR)/build/psx/dbgtext.bin" PSX_SHOT_FRAMES=30 \
	    timeout 60 $(PCSX_REDUX) -cli -softgpu -testmode -run -no-gui-log \
	    -bios "$(PSX_BIOS)" -exe $< \
	    -dofile libpsx/tools/redux_shot.lua >/dev/null 2>&1; \
	 if [ ! -f build/psx/dbgtext.bin ]; then echo "FAIL: no framebuffer captured"; exit 1; fi; \
	 ./build/skj-shotcheck build/psx/dbgtext.bin --ppm build/psx/dbgtext.ppm \
	    --find 255 255 255 24 --at 8 8 0 0 16 24; \
	 rc=$$?; \
	 if [ $$rc -eq 0 ]; then echo "PASS: dbgtext (white glyphs present, background intact)"; \
	 else echo "FAIL: dbgtext framebuffer (shotcheck $$rc)"; exit 1; fi

## Golden-audio tier: capture what the SPU actually plays and check its pitch.
## audio_tone keys a 440 Hz sample on voice 0 for the whole run; SDL's disk audio
## driver writes the emulator's output stream to a file (no sound device, no
## window), and wavcheck measures the dominant frequency.  This verifies the SPU
## upload, the voice key-on, and the ADPCM playback produce the right tone, not
## just a clean exit.  DISPLAY is cleared as with the framebuffer tier.  Opt-in.
test-psx-audio-redux: build/psx/audio_tone.exe build/skj-wavcheck | build/psx
	@if ! command -v $(PCSX_REDUX) >/dev/null 2>&1; then \
	    echo "SKIP: PCSX-Redux ($(PCSX_REDUX)) not found"; exit 0; fi; \
	 if [ -z "$(PSX_BIOS)" ] || [ ! -f "$(PSX_BIOS)" ]; then \
	    echo "SKIP: no PlayStation BIOS (pass PSX_BIOS=/path/to/scphXXXX.bin)"; exit 0; fi; \
	 rm -f build/psx/audio.raw; \
	 env -u DISPLAY -u WAYLAND_DISPLAY \
	    SDL_AUDIO_DRIVER=disk SDL_AUDIO_DISK_OUTPUT_FILE="$(CURDIR)/build/psx/audio.raw" \
	    SDL_AUDIO_DISK_TIMESCALE=0.0 \
	    timeout 60 $(PCSX_REDUX) -cli -softgpu -testmode -run -no-gui-log \
	    -bios "$(PSX_BIOS)" -exe $< >/dev/null 2>&1; \
	 if [ ! -s build/psx/audio.raw ]; then echo "FAIL: no audio captured"; exit 1; fi; \
	 ./build/skj-wavcheck build/psx/audio.raw --expect 440 --tol 12; \
	 rc=$$?; \
	 if [ $$rc -eq 0 ]; then rm -f build/psx/audio.raw; \
	    echo "PASS: audio_tone (SPU output is 440 Hz)"; \
	 else echo "FAIL: audio_tone (wavcheck $$rc, kept build/psx/audio.raw)"; exit 1; fi

## ADSR/reverb tier: one run checks both halves of the sample.  The emulator's
## exit code is the make_adsr self-check (non-zero means the envelope bit layout
## regressed), and SDL's disk audio driver captures the SPU output so wavcheck
## confirms the tone still comes through at 440 Hz with the envelope and reverb
## paths exercised.  DISPLAY is cleared.  Opt-in.
test-psx-env-redux: build/psx/env_tone.exe build/skj-wavcheck | build/psx
	@if ! command -v $(PCSX_REDUX) >/dev/null 2>&1; then \
	    echo "SKIP: PCSX-Redux ($(PCSX_REDUX)) not found"; exit 0; fi; \
	 if [ -z "$(PSX_BIOS)" ] || [ ! -f "$(PSX_BIOS)" ]; then \
	    echo "SKIP: no PlayStation BIOS (pass PSX_BIOS=/path/to/scphXXXX.bin)"; exit 0; fi; \
	 rm -f build/psx/env.raw; \
	 env -u DISPLAY -u WAYLAND_DISPLAY \
	    SDL_AUDIO_DRIVER=disk SDL_AUDIO_DISK_OUTPUT_FILE="$(CURDIR)/build/psx/env.raw" \
	    SDL_AUDIO_DISK_TIMESCALE=0.0 \
	    timeout 60 $(PCSX_REDUX) -cli -softgpu -testmode -run -no-gui-log \
	    -bios "$(PSX_BIOS)" -exe $< >/dev/null 2>&1; \
	 rc=$$?; \
	 if [ $$rc -ne 0 ]; then echo "FAIL: env_tone make_adsr self-check (exit $$rc)"; exit 1; fi; \
	 if [ ! -s build/psx/env.raw ]; then echo "FAIL: no audio captured"; exit 1; fi; \
	 ./build/skj-wavcheck build/psx/env.raw --expect 440 --tol 12; \
	 wc=$$?; \
	 if [ $$wc -eq 0 ]; then rm -f build/psx/env.raw; \
	    echo "PASS: env_tone (make_adsr packs correctly, reverbed tone is 440 Hz)"; \
	 else echo "FAIL: env_tone (wavcheck $$wc, kept build/psx/env.raw)"; exit 1; fi

## Structural validation of the CD-audio sample's PS-EXE, always-on (the
## functional command run is test-psx-cdda-redux).
test-psx-cdda: build/psx/cdda.exe
	@OBJDUMP="$(MIPS_OBJDUMP)" sh tests/run-psx-tests.sh $<

## CD-audio command tier: boot the sample with the two-track CD-DA disc and
## self-check by exit code that the CD-audio command path drives the controller
## correctly: the table of contents reports the audio track, a CD-DA track plays
## with an acknowledgement, and the XA streaming commands are accepted.  Headless
## PCSX-Redux routes CD-DA audio to the host sound device rather than the SPU
## output the capture tier records, so this checks the protocol, not the tone (a
## host monitor capture confirms the sound by hand).  SDL audio is sent to the
## null sink so nothing plays during the run.  Needs genisoimage, PCSX-Redux,
## and a BIOS.  Opt-in.
test-psx-cdda-redux: build/psx/cdda.exe | build/psx
	@if [ -z "$(ISOGEN)" ]; then echo "SKIP: genisoimage not found"; exit 0; fi; \
	 $(MAKE) build/psx/cdda.cue >/dev/null || exit 1; \
	 if ! command -v $(PCSX_REDUX) >/dev/null 2>&1; then echo "SKIP: PCSX-Redux not found"; exit 0; fi; \
	 if [ -z "$(PSX_BIOS)" ] || [ ! -f "$(PSX_BIOS)" ]; then echo "SKIP: no PlayStation BIOS"; exit 0; fi; \
	 env -u DISPLAY -u WAYLAND_DISPLAY \
	    SDL_AUDIO_DRIVER=disk SDL_AUDIODRIVER=disk \
	    SDL_AUDIO_DISK_OUTPUT_FILE=/dev/null SDL_AUDIO_DISK_TIMESCALE=0.0 \
	    timeout 60 $(PCSX_REDUX) -cli -softgpu -testmode -run -no-gui-log \
	    -bios "$(PSX_BIOS)" -iso build/psx/cdda.cue -exe $< >/dev/null 2>&1; \
	 rc=$$?; \
	 if [ $$rc -eq 0 ]; then echo "PASS: cdda (TOC read, CD-DA track plays, XA stream commands accepted)"; \
	 else echo "FAIL: cdda (exit $$rc names the failed step)"; exit 1; fi

## Structural validation of the memory-card sample's PS-EXE, always-on (the
## functional run is test-psx-mc-redux).
test-psx-mc: build/psx/mctest.exe
	@OBJDUMP="$(MIPS_OBJDUMP)" sh tests/run-psx-tests.sh $<

## Memory-card exit-code tier: boot the sample against a throwaway card image and
## self-check the whole save path by exit code: a raw frame round-trip, then
## format, a single-block and a multi-block save, list, load-and-compare, and
## delete.  It runs with -memcard1 pointing at build/psx/mc_test.mcd (created
## fresh each run) so it never touches the user's real PCSX-Redux card, which a
## format would wipe.  Opt-in.
test-psx-mc-redux: build/psx/mctest.exe | build/psx
	@if ! command -v $(PCSX_REDUX) >/dev/null 2>&1; then \
	    echo "SKIP: PCSX-Redux ($(PCSX_REDUX)) not found"; exit 0; fi; \
	 if [ -z "$(PSX_BIOS)" ] || [ ! -f "$(PSX_BIOS)" ]; then \
	    echo "SKIP: no PlayStation BIOS (pass PSX_BIOS=/path/to/scphXXXX.bin)"; exit 0; fi; \
	 rm -f build/psx/mc_test.mcd; \
	 env -u DISPLAY -u WAYLAND_DISPLAY timeout 60 $(PCSX_REDUX) -testmode -cli -run -no-gui-log \
	    -memcard1 "$(CURDIR)/build/psx/mc_test.mcd" \
	    -bios "$(PSX_BIOS)" -exe $< >/dev/null 2>&1; \
	 rc=$$?; \
	 if [ $$rc -eq 0 ]; then echo "PASS: mctest (frame I/O + save-file manager verified)"; \
	 else echo "FAIL: mctest (exit $$rc names the failed step)"; exit 1; fi

## Structural validation of the analog-pad sample's PS-EXE, always-on (the
## functional run is test-psx-pad-redux).
test-psx-pad: build/psx/padtest.exe
	@OBJDUMP="$(MIPS_OBJDUMP)" sh tests/run-psx-tests.sh $<

## Controller exit-code tier: enable analog mode and read the pad the emulator
## presents.  Headless PCSX-Redux presents a digital pad with no stick input, so
## this checks that the analog-enable sequence runs, a controller answers with a
## valid id, the length-aware read parses the response, and the stick axes read
## centered.  Opt-in.
test-psx-pad-redux: build/psx/padtest.exe
	@if ! command -v $(PCSX_REDUX) >/dev/null 2>&1; then \
	    echo "SKIP: PCSX-Redux ($(PCSX_REDUX)) not found"; exit 0; fi; \
	 if [ -z "$(PSX_BIOS)" ] || [ ! -f "$(PSX_BIOS)" ]; then \
	    echo "SKIP: no PlayStation BIOS (pass PSX_BIOS=/path/to/scphXXXX.bin)"; exit 0; fi; \
	 env -u DISPLAY -u WAYLAND_DISPLAY timeout 60 $(PCSX_REDUX) -testmode -cli -run -no-gui-log \
	    -bios "$(PSX_BIOS)" -exe $< >/dev/null 2>&1; \
	 rc=$$?; \
	 if [ $$rc -eq 0 ]; then echo "PASS: padtest (analog enable + length-aware read + centered sticks)"; \
	 else echo "FAIL: padtest (exit $$rc names the failed check)"; exit 1; fi

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
build/mips-nr/scm_%.s: tests/scm_%.scm build/skj-sc-mips | build/mips-nr
	SKJ_MIPS_NOREORDER=1 ./build/skj-sc-mips -o $@ $<

build/mips-nr/%.o: build/mips-nr/%.s
	$(MIPS_AS) -march=mips1 -EL -o $@ $<

build/mips-nr/start.o: runtime/start_mips.S | build/mips-nr
	$(MIPS_AS) -march=mips1 -EL -o $@ $<

build/mips-nr/%: build/mips-nr/%.o build/mips-nr/start.o
	$(MIPS_LD) -o $@ build/mips-nr/start.o $<

check-mips-noreorder: build/skj-sc-mips $(MIPS_SCM_TESTS:tests/%.scm=build/mips-nr/%)
	@sh tests/run-tests.sh "$(QEMU_MIPS)" build/mips-nr

## The C compiler suite under the noreorder scheduler (int, float, i64,
## _Float16 coverage).  Same tools, SKJ_MIPS_NOREORDER exported.
check-cc-mips-noreorder: build/skj-cc-mips build/mips/start.o build/mips/half.o build/mips/va_mips.o
	$(call run_cc_suite,$(CURDIR)/build/skj-cc-mips,$(MIPS_AS) -march=mips1 -EL -o,$(MIPS_LD) -o,$(QEMU_MIPS),$(CURDIR)/build/mips/start.o,$(CURDIR)/build/mips/half.o $(CURDIR)/build/mips/va_mips.o,$(CURDIR)/build/cc-mips-nr,$(MIPS_EXCLUDE),SKJ_MIPS_NOREORDER=1)

## The C compiler suite compiled soft-float: skj-cc-mips-sf emits no cop-1
## instruction and the programs link softfloat.o beside half.o.  The float
## tests (t054/t057/t060/t070) exercise the soft-float path end to end.
check-cc-mips-sf: build/skj-cc-mips-sf build/mips/start.o build/mips/half.o build/mips/softfloat.o
	$(call run_cc_suite,$(CURDIR)/build/skj-cc-mips-sf,$(MIPS_AS) -march=mips1 -EL -o,$(MIPS_LD) -o,$(QEMU_MIPS),$(CURDIR)/build/mips/start.o,$(CURDIR)/build/mips/half.o $(CURDIR)/build/mips/softfloat.o,$(CURDIR)/build/cc-mips-sf,$(MIPS_EXCLUDE) $(VA_STDARG_EXCLUDE))

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
check-mips-all: check-mips check-mips-noreorder check-mips-emu test-mips-fs-cap \
                test-mips-sandbox test-mips-selfhost test-mips-gdb test-mips-backtrace \
                test-i64-mips test-ops-mips test-fpu-mips test-f32-mips \
                check-cc-mips check-cc-mips-noreorder \
                check-mipsas check-cc-mips-skj test-mips-archive test-mips-psexe \
                test-psx-gpu test-psx-tex test-psx-rot test-psx-alpha test-psx-cd test-psx-audio test-psx-timconv test-psx-adpcm test-psx-m2 test-psx-assets \
                test-psx-fixed test-psx-game test-psx-lang test-psx-units test-psx-sprite-lang test-psx-gte test-psx-timer test-psx-cdda test-psx-mc test-psx-pad test-psx-shade test-psx-env test-psx-dbgtext test-psx-cube-lang test-psx-kasm test-psx-cube-driver \
                test-cc-mips-asm check-mips-sf test-threaded-mips
	@echo "All MIPS test suites passed."

check-cpp: build/skj-cpp
	@sh tests/run-cpp-tests.sh

## Smoke test: full in-tree toolchain (front end -> skj-as -> skj-ld -> qemu)
check-smoke: build/skj-sc build/skj-mooc build/skj-cc build/skj-as build/skj-ld
	@sh tests/smoke.sh "$(QEMU)"

## skj-as validation: assemble everything with skj-as instead of m68k-linux-gnu-as
check-as: build/skj-sc build/skj-mooc build/skj-pc build/skj-as
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

## ELF64 linkers end to end: assemble a fixture with the in-tree ELF64
## assembler, link it with the in-tree ELF64 linker, and run the result (x86-64
## natively or under qemu-x86_64; AArch64 under qemu-aarch64).  The fixtures
## cover the PC-relative, absolute-32 and absolute-64 relocation forms each
## target emits; the program writes "ok" and exits 42.
test-ld-x86-64: build/skj-as-x86-64 build/skj-ld-x86-64 | build
	@./build/skj-as-x86-64 -o build/ld_hello_x86_64.o tests/ld/hello_x86_64.s
	@./build/skj-ld-x86-64 -o build/ld_hello_x86_64 build/ld_hello_x86_64.o
	@out=`$(QEMU_X64) ./build/ld_hello_x86_64`; ec=$$?; \
	 if [ "$$out" = ok ] && [ "$$ec" = 42 ]; then \
	     echo "PASS: skj-ld-x86-64 links a runnable ET_EXEC (ok, exit 42)"; \
	 else echo "FAIL: skj-ld-x86-64 (out '$$out', exit $$ec)"; exit 1; fi

test-ld-arm64: build/skj-as-aarch64 build/skj-ld-arm64 | build
	@./build/skj-as-aarch64 -o build/ld_hello_arm64.o tests/ld/hello_arm64.s
	@./build/skj-ld-arm64 -o build/ld_hello_arm64 build/ld_hello_arm64.o
	@out=`$(QEMU_A64) ./build/ld_hello_arm64`; ec=$$?; \
	 if [ "$$out" = ok ] && [ "$$ec" = 42 ]; then \
	     echo "PASS: skj-ld-arm64 links a runnable ET_EXEC (ok, exit 42)"; \
	 else echo "FAIL: skj-ld-arm64 (out '$$out', exit $$ec)"; exit 1; fi

## Full qemu matrix across every supported architecture: the per-language
## suites, plus the IR-builder integration tests (fpu / i64 / unsigned ops).
## ColdFire (qemu-m68k), RISC-V (qemu-riscv32, hardware float), x86-64
## (qemu-x86_64), AArch64 (qemu-aarch64).
## Every skj-* tool recognizes -h / --help: prints its usage and exits 0.  One
## representative per distinct entry point (the cross variants share a main.c
## with the host tool, and skj-kpc covers the kpcom seed).  This keeps the -h
## surface uniform as tools are added.
HELP_TOOLS := skj-sc skj-mooc skj-pc skj-cc skj-cpp skj-jit \
              skj-as skj-as-rv skj-as-mips skj-as-x86-64 skj-as-aarch64 skj-ld skj-ld-rv skj-ld-mips skj-ar \
              skj-run skj-addr2line skj-dbg skj-kp-jit skj-kp-patrol \
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

## check-all: the host-native cross path.  Every compiler here runs natively on
## the host and emits target code; only test programs run under an emulator.  The
## in-process JITs that run a compiler itself under emulation (the cross
## skj-jit-* and skj-kp-jit-*) live in check-emu-all, not here.  check-jit stays:
## it is the native host-arch (x86-64) JIT, the peer of check-kp-jit.
check-all: check check-cc check-cc-x86-64 check-cc-arm64 check-cc-rv check-cpp \
           test-cc-cf-asm test-cc-rv-asm test-cc-x86-64-asm test-cc-arm64-asm \
           check-rv check-x86-64 check-arm64 \
           check-x86-oracle check-arm64-oracle check-rv-oracle check-jit \
           test-jit-trap \
           test-jit-arena test-fs \
           test-gc test-coro test-threads \
           test-fpu test-fpu-rv test-fpu-x86-64 test-fpu-arm64 \
           test-f32-diff \
           test-i64 test-i64-rv test-i64-x86-64 test-i64-arm64 \
           test-ops test-ops-rv test-ops-x86-64 test-ops-arm64 \
           test-threaded test-threaded-rv test-threaded-x86-64 test-threaded-arm64 \
           check-rvas check-x86-64as check-aarch64as test-ld-x86-64 test-ld-arm64 \
           test-cc-driver-x86-64 test-cc-driver-arm64 check-cc-rv-skj \
           test-cc-rv-loc test-kp-rv-loc test-kp-rv-dwarf test-kp-rv-trap-backtrace test-pc-loc test-moo-loc \
           test-dwarf-line test-rv-stt-func test-rv-debug-sections \
           test-rv-debug-link test-rv-addr2line test-rv-backtrace test-rv-abort-backtrace \
           test-bt-loop test-bt-trim test-rv-wild-backtrace \
           test-cf-addr2line test-mips-addr2line test-mips-backtrace \
           test-cf-wild-backtrace test-cf-abort-backtrace \
           test-kp-cf-dwarf test-kp-cf-trap-backtrace \
           test-rv-dbg test-rv-dbg-multi test-rv-gdb test-rv-gdb-fpwatch test-mips-gdb test-mips-gdb-fpwatch test-rv-sandbox-io test-rv-libc-io test-rv-stat test-rv-fileops test-rv-dirs test-cf-stat test-cf-fileops test-cf-dirs test-rv-fs-cap test-cf-fs-cap test-mips-fs-cap test-rv-fs-dev test-cf-fs-dev test-rv-fs-rom test-cf-fs-rom test-rv-fs-9p test-cf-fs-9p check-emu-posix test-rv-arc4random test-cf-arc4random test-rv-selfhost test-cf-selfhost test-mips-selfhost test-rv-selfhost-toolchain test-cf-selfhost-toolchain test-rv-selfhost-kpc test-cf-selfhost-kpc check-selfhost-all test-help \
           check-cc-rv-psabi test-cc-rv-psabi-interop \
           test-cc-rv-psabi-gcc-stock test-cc-rv-psabi-archive \
           test-cc-rv-psabi-struct test-cc-driver check-libc \
           check-kp \
           check-kp-frontend check-kp-jit check-kp-aot check-kp-asm check-kp-corpus check-kp-corpus-cf check-kp-multi check-kp-files check-kp-textfile check-kp-dirs check-kp-stdlib check-kp-stdlib-free check-kp-stdlib-free-cf check-kp-io check-kp-inc
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

.PHONY: help check-pc-host check-pc-arm64 all install uninstall release-check check check-rv run-rv test-rv-irq test-rv-run test-guest-mmap test-rv-sandbox test-rv-xlang-sandbox test-mips-sandbox test-cf-sandbox test-rv-spawn-argv test-rv-spawn-wait test-rv-spawn-kill test-rv-spawn-backtrace test-rv-spawn-mounts test-cf-spawn-mounts test-rv-spawn-pipe test-cf-spawn-pipe test-rv-jsh test-cf-jsh test-mips-jsh test-x86-64-jsh test-arm64-jsh test-rv-ps test-rv-vdso test-cf-vdso test-mips-vdso test-rv-mem-root test-sched-embed test-hv-x86-64-vdso test-hv-arm64-vdso test-rv-env test-cf-env test-hv-x86-64-env test-rv-decode-cache test-rv-expand test-rv-bus test-rv-csr test-rv-decode test-rv-fp check-archtest check-rv32 test-rv32-apps test-rv32-unit test-rv32-program test-rv32-lockstep test-rv32-fuzz audit-rv32-coverage audit-rv32-icov audit-rv32-mutants check-emu check-rv-emu check-mips-emu check-emu-all check-emu-san bench bench-cf bench-rv bench-accept test-fpu-emu test-i64-emu test-ops-emu check-x86-64 check-arm64 check-x86-oracle check-arm64-oracle check-rv-oracle check-jit check-jit-arm64 check-jit-rv check-jit-arm64-disasm check-jit-rv-disasm test-jit-trap test-jit-trap-arm64 test-jit-arm64-high check-all check-slow check-cc check-cc-x86-64 check-cc-arm64 check-cc-rv check-cpp check-as check-rvas check-x86-64as check-aarch64as test-ld-x86-64 test-ld-arm64 test-cc-driver-x86-64 test-cc-driver-arm64 check-cc-rv-skj check-cc-rv-psabi test-cc-rv-psabi-interop test-cc-rv-psabi-gcc-stock test-cc-rv-psabi-archive test-cc-rv-psabi-struct test-cc-driver check-libc check-libc-cf check-libc-mips check-libc-rv check-libc-x86-64 check-libc-arm64 check-mipsas check-cc-mips-skj test-cc-mips-asm test-cc-cf-asm test-cc-rv-asm test-cc-x86-64-asm test-cc-arm64-asm check-mips-all check-smoke test-gc test-coro test-threads test-jit-arena test-fs check-fs-free test-fpu test-fpu-rv test-fpu-x86-64 test-fpu-arm64 test-f32-rv f32-oracle f32-golden-check test-f32-diff test-f32-diff-rv test-f32-diff-x86-64 test-f32-diff-arm64 test-f32-diff-mips test-f32-diff-cf test-torture-diff test-i64 test-i64-rv test-i64-x86-64 test-i64-arm64 test-ops test-ops-rv test-ops-x86-64 test-ops-arm64 test-threaded test-threaded-rv test-threaded-x86-64 test-threaded-arm64 test-threaded-mips test-parse bootstrap-fpc check-kp-frontend check-kp-jit check-kp-aot check-kp-asm check-kp-jit-rv check-kp-jit-arm64 check-kp-aot-rv check-kp-aot-arm64 check-kp-free check-kp-free-cf check-kp-corpus check-kp-corpus-san check-kp-corpus-rv check-kp-corpus-arm64 check-kp-corpus-cf check-kp-multi check-kp-multi-rv check-kp-multi-arm64 check-kp-files check-kp-textfile check-kp-dirs check-kp-stdlib check-kp-stdlib-free check-kp-stdlib-free-cf check-kp-io check-kp-io-rv check-kp-io-arm64 check-kp-io-free check-kp-inc check-kp-inc-rv check-kp-inc-arm64 check-kp-inc-free test-help test-rv-sandbox-io test-rv-libc-io test-rv-stat test-rv-fileops test-rv-dirs test-cf-stat test-cf-fileops test-cf-dirs test-rv-fs-cap test-cf-fs-cap test-mips-fs-cap test-rv-fs-dev test-cf-fs-dev test-rv-fs-rom test-cf-fs-rom test-rv-fs-9p test-cf-fs-9p check-emu-posix test-rv-arc4random test-cf-arc4random test-rv-selfhost test-rv-selfhost-full test-cf-selfhost test-cf-selfhost-full test-mips-selfhost test-rv-selfhost-toolchain test-cf-selfhost-toolchain test-rv-selfhost-kpc test-cf-selfhost-kpc test-rv-selfhost-kpc-full test-cf-selfhost-kpc-full check-selfhost-all test-rv-abort-backtrace test-bt-loop test-bt-trim test-rv-wild-backtrace test-kp-rv-dwarf test-kp-rv-trap-backtrace test-cf-addr2line test-mips-addr2line test-mips-backtrace test-cf-wild-backtrace test-cf-abort-backtrace test-kp-cf-dwarf test-kp-cf-trap-backtrace test-mips-gdb test-rng test-rng-glue test-sched-glue test-vmm test-hv-x86-64 test-hv-arm64 test-hv-gdb test-hv-gdb-fpwatch test-hv-preempt test-hv-parallel test-hv-x86-64-pump check-libc-hv-x86-64 check-libc-hv-arm64 test-macho-arm64 test-macho-arm64-dwarf check-cc-macho-arm64 check-libc-macho-arm64 check-vendor coverage-codegen clean FORCE
