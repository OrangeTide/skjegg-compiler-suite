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
## UBSan only, for tools that map JITed code into low memory (the JIT driver),
## where ASan's shadow reservation collides with that mapping.  UBSan needs no
## shadow and still catches the shift/overflow/out-of-bounds-index UB.
UBSAN_CFLAGS := -fsanitize=undefined -fno-sanitize-recover=all -g

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
QEMU    ?= qemu-m68k
RV_AS   ?= riscv64-linux-gnu-as
RV_LD   ?= riscv64-linux-gnu-ld
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
QEMU_MIPS ?= qemu-mipsel
SPIM ?= spim
A64_AS  ?= aarch64-linux-gnu-as
A64_LD  ?= aarch64-linux-gnu-ld
A64_CC  ?= aarch64-linux-gnu-gcc
QEMU_A64 ?= qemu-aarch64
# The in-process RV32 JIT driver is a hosted program (mmap/stdio/malloc), which
# the rv64 riscv64-linux-gnu toolchain cannot build for rv32 (no ilp32 glibc
# multilib).  zig cc bundles an rv32 libc, so it cross-builds the driver; it runs
# under qemu-riscv32.  Override RVJIT_CC if a native rv32 hosted toolchain exists.
RVJIT_CC ?= zig cc -target riscv32-linux-musl
X64_ASM ?= nasm
X64_LD  ?= ld
QEMU_X64 ?= qemu-x86_64

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
# skj-cc-arm64 / skj-exc-arm64 use this; the stack-ABI arm64 tools (tinc/scheme)
# and the i64/ops/fpu integration tests keep BE_A64 (arm64_emit.c), the same
# split x86 has between BE_X64_SEL and BE_X64.
BE_A64_SEL := backend/regalloc_arm64.c backend/arm64_select.c backend/arm64_mc_text.c
# The unified x86-64 (LP64) backend: the shared instruction selector over the
# NASM text sink, driven by the AOT driver.  skj-cc-x86-64 uses this; the other
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

TOOLS := build/skj-tinc build/skj-tinc-rv build/skj-tinc-mips build/skj-tinc-arm64 \
         build/skj-sc build/skj-sc-rv build/skj-sc-mips build/skj-sc-arm64 \
         build/skj-mooc build/skj-pc build/skj-as build/skj-ld \
         build/skj-as-rv build/skj-ld-rv build/skj-as-mips build/skj-ld-mips build/skj-ar \
         build/skj-cpp build/skj-cc build/skj-cc-x86-64 build/skj-cc-arm64 \
         build/skj-cc-rv build/skjcc build/skj-run \
         build/skj-jit build/skj-kp-jit build/skj-kp-patrol \
         build/skj-kp build/libkpaot.a build/skj-kpc

all: $(TOOLS)

build/skj-tinc: $(SRC) ir/ir.h tinc/tinc.h | build
	$(CC) $(CFLAGS) -Iir -Itinc -o $@ $(SRC)

build/skj-tinc-rv: $(SRC_RV) ir/ir.h tinc/tinc.h | build
	$(CC) $(CFLAGS) -Iir -Itinc -o $@ $(SRC_RV)

build/skj-tinc-mips: $(SRC_MIPS) ir/ir.h tinc/tinc.h | build
	$(CC) $(CFLAGS) -Iir -Itinc -o $@ $(SRC_MIPS)

build/skj-tinc-arm64: $(SRC_A64) ir/ir.h tinc/tinc.h | build
	$(CC) $(CFLAGS) -Iir -Itinc -o $@ $(SRC_A64)

build/skj-tinc-x86-64: $(SRC_X64) ir/ir.h tinc/tinc.h | build
	$(CC) $(CFLAGS) -DX86_BITS=64 -Iir -Itinc -o $@ $(SRC_X64)

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
	$(CC) $(CFLAGS) -DX86_BITS=64 -DCC_LP64 -DCC_PSABI -DCC_STRUCT_ABI $(SKJ_INC_DEF) -Icc -Icpp -Iir -Ibackend -o $@ $(CC_SRC_X64)

build/skj-cc-arm64: $(CC_SRC_A64) cc/cc.h cpp/cpp.h cpp/internal.h ir/ir.h backend/arm64_mc.h backend/arm64_select.h backend/arm64_mc_text.h | build
	$(CC) $(CFLAGS) -DCC_LP64 -DCC_PSABI -DCC_ARM64 $(SKJ_INC_DEF) -Icc -Icpp -Iir -o $@ $(CC_SRC_A64)

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
	$(CC) $(CFLAGS) -DX86_BITS=64 -DCC_PSABI -DCC_STRUCT_ABI $(SKJ_INC_DEF) -Icc -Icpp -Iir -Ijit -Ibackend -o $@ $(JIT_SRC)

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
          kobold/co_shim.c runtime/co_switch_x86.S kobold/kp_host.c \
          jit/jit_common.c jit/jit_arena.c jit/jit_x86.c jit/emit_x86.c jit/code.c \
          jit/call_guest_x86.S \
          backend/x86_select.c backend/regalloc_x86.c $(IR_SRC)
KP_HDRS := kobold/kpm.h kobold/str.h kobold/dynarr.h kobold/map.h kobold/rt_glue.h kobold/obj.h kobold/co.h \
           kobold/handle.h kobold/lowmem.h kobold/kp_host.h jit/jit_common.h jit/jit_arena.h \
           jit/jit_x86.h jit/emit_x86.h jit/code.h backend/mc.h backend/x86_select.h ir/ir.h
KP_CFLAGS := $(CFLAGS) -DX86_BITS=64 -DKP_FREESTANDING -DKP_REAL=double -Iir -Ijit -Ibackend -Ikobold

## co.c is vendored unchanged from the Kobold tree, which builds with -Wall -Wextra
## (no -Wpedantic).  Compile it with that same set so it stays byte-identical and
## re-syncable, while the rest of the kobold/ glue keeps Skjegg's full -Wpedantic.
build/kp_co.o: kobold/co.c kobold/co.h kobold/handle.h kobold/lowmem.h Makefile | build
	$(CC) -std=c99 -O2 -Wall -Wextra -Wno-unused-parameter -Iir -Ijit -Ibackend -Ikobold -c kobold/co.c -o $@

## bin2c embeds the bootstrap seed as a C byte array (build/kpcom_kpm.c, with a
## matching .h generated as a side effect) so skj-kp-jit* carry the Kobold front
## end in the binary.  Then 'skj-kp-jit foo.pas' JITs the embedded front end,
## compiles the source in process through its `compile` export, and runs the
## produced module.  The embedded array is arch-neutral, so one rule serves all
## three JIT tools.  KPCOM_KPM in the environment overrides it for dev iteration.
build/bin2c: kobold/bin2c.c Makefile | build
	$(CC) $(CFLAGS) -o $@ kobold/bin2c.c

build/kpcom_kpm.c: prebuild/kpcom.kpm build/bin2c | build
	build/bin2c kpcom_kpm prebuild/kpcom.kpm build/kpcom_kpm.c build/kpcom_kpm.h

build/skj-kp-jit: kobold/jit_main.c kobold/frontend.c build/kpcom_kpm.c $(KP_RT) build/kp_co.o kobold/frontend.h $(KP_HDRS) Makefile | build
	$(CC) $(KP_CFLAGS) -Ibuild -o $@ kobold/jit_main.c kobold/frontend.c build/kpcom_kpm.c $(KP_RT) build/kp_co.o -lm

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
            kobold/co_shim.c runtime/co_switch_rv.S kobold/kp_host.c \
            jit/jit_common.c jit/jit_arena.c jit/jit_rv.c jit/emit_rv.c jit/code.c \
            jit/call_guest_rv.S jit/rv_jit_i64.S \
            backend/rv_select.c backend/regalloc_rv.c $(IR_SRC)
KP_HDRS_RV := kobold/kpm.h kobold/str.h kobold/dynarr.h kobold/map.h kobold/rt_glue.h kobold/obj.h kobold/co.h \
              kobold/handle.h kobold/lowmem.h kobold/kp_host.h jit/jit_common.h jit/jit_arena.h \
              jit/emit_rv.h jit/code.h backend/rv_mc.h backend/rv_select.h ir/ir.h
KP_CFLAGS_RV := $(CFLAGS) -DKP_FREESTANDING -DKP_REAL=double -Iir -Ijit -Ibackend -Ikobold

## co.c built for rv with the same vendored-warning set as the x86 kp_co.o.
build/kp_co_rv.o: kobold/co.c kobold/co.h kobold/handle.h kobold/lowmem.h Makefile | build
	$(RVJIT_CC) -std=c99 -O2 -Wall -Wextra -Wno-unused-parameter -Iir -Ijit -Ibackend -Ikobold -c kobold/co.c -o $@

build/skj-kp-jit-rv: kobold/jit_main.c kobold/frontend.c build/kpcom_kpm.c $(KP_RT_RV) build/kp_co_rv.o kobold/frontend.h $(KP_HDRS_RV) Makefile | build
	$(RVJIT_CC) $(KP_CFLAGS_RV) -Ibuild -o $@ kobold/jit_main.c kobold/frontend.c build/kpcom_kpm.c $(KP_RT_RV) build/kp_co_rv.o -lm

## skj-kp-jit-arm64: the in-process AArch64 JIT for Kobold modules, the cross-arch
## counterpart of skj-kp-jit.  Same driver, ported runtime, and selector, over the
## AArch64 byte sink (jit_arm64/emit_arm64), call trampoline, and coroutine switch.
## i64 is native (no stack-ABI helpers) and floats ride the AAPCS64 d-registers, so
## no ABI bridge is needed, unlike rv32.  On this LP64 host a guest address must
## still be below 4GB, so the guest heap and coroutine stacks are mapped low (see
## rt_host.c/co_shim.c).  Cross-built with the arm64 toolchain and run under
## qemu-aarch64, which executes the JIT-generated code.
KP_RT_A64 := kobold/kpm.c kobold/str_core.c kobold/str_fmt.c kobold/dynarr.c kobold/map.c kobold/rt_host.c \
             kobold/co_shim.c runtime/co_switch_arm64.S kobold/kp_host.c \
             jit/jit_common.c jit/jit_arena.c jit/jit_arm64.c jit/emit_arm64.c jit/code.c \
             jit/call_guest_arm64.S \
             backend/arm64_select.c backend/regalloc_arm64.c $(IR_SRC)
KP_HDRS_A64 := kobold/kpm.h kobold/str.h kobold/dynarr.h kobold/map.h kobold/rt_glue.h kobold/obj.h kobold/co.h \
               kobold/handle.h kobold/lowmem.h kobold/kp_host.h jit/jit_common.h jit/jit_arena.h \
               jit/emit_arm64.h jit/code.h backend/arm64_mc.h backend/arm64_select.h ir/ir.h
KP_CFLAGS_A64 := $(CFLAGS) -DKP_FREESTANDING -DKP_REAL=double -Iir -Ijit -Ibackend -Ikobold

## co.c built for arm64 with the same vendored-warning set as the x86 kp_co.o.
build/kp_co_a64.o: kobold/co.c kobold/co.h kobold/handle.h kobold/lowmem.h Makefile | build
	$(A64_CC) -std=c99 -O2 -Wall -Wextra -Wno-unused-parameter -Iir -Ijit -Ibackend -Ikobold -c kobold/co.c -o $@

build/skj-kp-jit-arm64: kobold/jit_main.c kobold/frontend.c build/kpcom_kpm.c $(KP_RT_A64) build/kp_co_a64.o kobold/frontend.h $(KP_HDRS_A64) Makefile | build
	$(A64_CC) $(KP_CFLAGS_A64) -Ibuild -o $@ kobold/jit_main.c kobold/frontend.c build/kpcom_kpm.c $(KP_RT_A64) build/kp_co_a64.o -lm

## skj-kp: the x86-64 ahead-of-time compiler for Kobold modules.  It reads a KPM1
## module and writes NASM assembly through the same selector the JIT drives, over
## the text sink (backend/mc_text.c) and AOT driver (backend/x86_aot.c).  The output
## is assembled with nasm and linked -no-pie against libkpaot.a:
##     skj-kp prog.kpm -o prog.asm
##     nasm -f elf64 prog.asm -o prog.o
##     cc -no-pie prog.o build/libkpaot.a -o prog
## -no-pie keeps the program's code and data below 4GB, matching the 32-bit guest
## address model; libkpaot.a's start file adds a low-memory execution stack.
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
KP_AOT_RT := kobold/aot_rt.c kobold/kp_host.c kobold/str_core.c kobold/str_fmt.c kobold/dynarr.c \
             kobold/map.c kobold/rt_host.c kobold/co_shim.c \
             runtime/co_switch_x86.S jit/call_guest_x86.S
build/libkpaot.a: $(KP_AOT_RT) build/kp_co.o $(KP_HDRS) Makefile | build
	@rm -rf build/aot && mkdir -p build/aot
	@for f in $(KP_AOT_RT); do \
	    o=build/aot/`basename $$f | sed 's/\.[cS]$$/.o/'`; \
	    echo "  CC $$f"; \
	    $(CC) $(KP_CFLAGS) -c $$f -o $$o || exit 1; \
	done
	$(AR) rcs $@ build/aot/*.o build/kp_co.o

## skj-kp-rv / skj-kp-arm64: the cross-arch ahead-of-time compilers, the AOT
## counterparts of skj-kp-jit-rv / skj-kp-jit-arm64.  Each is a host binary that
## emits GNU-as assembly for its target through the same selector the matching JIT
## drives, over that target's text sink (rv_mc_text.c / arm64_mc_text.c); the cross
## toolchain then assembles and links it against the target runtime.  aot_main.c is
## arch-neutral; -DKP_AOT_RV only picks the rv entries of its guest-name remap (the
## soft-float real ABI bridge and the stack-ABI 64-bit multiply/shift helpers).
build/skj-kp-rv: kobold/aot_main.c kobold/kpm.c $(IR_SRC) $(BE_RV) kobold/kpm.h ir/ir.h \
                 backend/rv_mc_text.h backend/rv_select.h Makefile | build
	$(CC) $(CFLAGS) -DKP_AOT_RV -Iir -Ibackend -Ikobold -o $@ \
	    kobold/aot_main.c kobold/kpm.c $(IR_SRC) $(BE_RV)

build/skj-kp-arm64: kobold/aot_main.c kobold/kpm.c $(IR_SRC) $(BE_A64_SEL) kobold/kpm.h ir/ir.h \
                    backend/arm64_mc_text.h backend/arm64_select.h Makefile | build
	$(CC) $(CFLAGS) -Iir -Ibackend -Ikobold -o $@ \
	    kobold/aot_main.c kobold/kpm.c $(IR_SRC) $(BE_A64_SEL)

## libkpaot-rv.a / libkpaot-arm64.a: the target runtimes an AOT program links
## against, the cross-compiled siblings of libkpaot.a.  Same sources (the ported
## runtime plus the aot_rt.c start file) built with the target toolchain and its
## coroutine switch and call trampoline.  rv also carries jit/rv_jit_i64.S, whose
## stack-ABI 64-bit multiply and shifts the guest reaches through the skj_jit_*
## names (the register-ABI __divdi3 family and all FP math come from the toolchain
## runtime library at link).  arm64 needs neither: i64 and doubles are native.
KP_AOT_RT_RV := kobold/aot_rt.c kobold/kp_host.c kobold/str_core.c kobold/str_fmt.c kobold/dynarr.c \
                kobold/map.c kobold/rt_host.c kobold/co_shim.c \
                runtime/co_switch_rv.S jit/call_guest_rv.S jit/rv_jit_i64.S \
                kobold/rv_i64_aot_alias.S
build/libkpaot-rv.a: $(KP_AOT_RT_RV) build/kp_co_rv.o $(KP_HDRS_RV) Makefile | build
	@rm -rf build/aot-rv && mkdir -p build/aot-rv
	@for f in $(KP_AOT_RT_RV); do \
	    o=build/aot-rv/`basename $$f | sed 's/\.[cS]$$/.o/'`; \
	    echo "  RVCC $$f"; \
	    $(RVJIT_CC) $(KP_CFLAGS_RV) -c $$f -o $$o || exit 1; \
	done
	$(AR) rcs $@ build/aot-rv/*.o build/kp_co_rv.o

KP_AOT_RT_A64 := kobold/aot_rt.c kobold/kp_host.c kobold/str_core.c kobold/str_fmt.c kobold/dynarr.c \
                 kobold/map.c kobold/rt_host.c kobold/co_shim.c \
                 runtime/co_switch_arm64.S jit/call_guest_arm64.S
build/libkpaot-arm64.a: $(KP_AOT_RT_A64) build/kp_co_a64.o $(KP_HDRS_A64) Makefile | build
	@rm -rf build/aot-arm64 && mkdir -p build/aot-arm64
	@for f in $(KP_AOT_RT_A64); do \
	    o=build/aot-arm64/`basename $$f | sed 's/\.[cS]$$/.o/'`; \
	    echo "  A64CC $$f"; \
	    $(A64_CC) $(KP_CFLAGS_A64) -c $$f -o $$o || exit 1; \
	done
	$(AR) rcs $@ build/aot-arm64/*.o build/kp_co_a64.o

## skj-kpc / skj-kpc-rv / skj-kpc-arm64: the Kobold-Pascal front end as a native
## binary.  Each is the self-host bootstrap seed (prebuild/kpcom.kpm) AOT-compiled
## for that target by skj-kp* and linked against the matching libkpaot*.a, so the
## front end runs with no JIT and no fpc: 'skj-kpc --emit-kpm foo.pas' reads Kobold
## source and writes a KPM1 module to stdout.  The front end runs on the host for
## cross AOT (host skj-kpc emits the KPM1, skj-kp-rv/-arm64 then compile it to the
## target); skj-kpc-rv and skj-kpc-arm64 are the proof the seed AOT-compiles and
## runs on each backend, run under qemu.  All three emit byte-identical KPM1, being
## the same guest program.  x86 assembly is NASM (assembled separately); rv and
## arm64 are GNU-as, so the cross cc drives assembly and link in one step.
## The .s (seed codegen) and .o (assembly) are separate targets so a runtime-only
## change (libkpaot*.a) relinks without re-running the slow assembly of the large
## seed module.
build/kpcom.s: prebuild/kpcom.kpm build/skj-kp | build
	build/skj-kp prebuild/kpcom.kpm -o $@
build/kpcom.o: build/kpcom.s | build
	$(X64_ASM) -f elf64 $< -o $@
build/skj-kpc: build/kpcom.o build/libkpaot.a | build
	$(CC) -no-pie build/kpcom.o build/libkpaot.a -o $@ -lm

build/kpcom-rv.s: prebuild/kpcom.kpm build/skj-kp-rv | build
	build/skj-kp-rv prebuild/kpcom.kpm -o $@
build/kpcom-rv.o: build/kpcom-rv.s | build
	$(RVJIT_CC) -no-pie -c $< -o $@
build/skj-kpc-rv: build/kpcom-rv.o build/libkpaot-rv.a | build
	$(RVJIT_CC) -no-pie build/kpcom-rv.o build/libkpaot-rv.a -o $@ -lm

build/kpcom-arm64.s: prebuild/kpcom.kpm build/skj-kp-arm64 | build
	build/skj-kp-arm64 prebuild/kpcom.kpm -o $@
build/kpcom-arm64.o: build/kpcom-arm64.s | build
	$(A64_CC) -no-pie -c $< -o $@
build/skj-kpc-arm64: build/kpcom-arm64.o build/libkpaot-arm64.a | build
	$(A64_CC) -no-pie build/kpcom-arm64.o build/libkpaot-arm64.a -o $@ -lm

## ---- the bootstrap seed: regeneration and staleness guard ----
## prebuild/kpcom.kpm is the self-host bootstrap seed (stage0), the KPM1 module
## kpcom emits when it compiles its own source (kobold/kpcom.pas).  The source
## lives in kobold/ as a first-class front end; only the generated stage0 seed is
## a prebuilt artifact.  update-kpcom is the seed's only writer: it regenerates it
## by the self-host fixed point and rewrites the manifest.  check-kpcom is a
## source-based staleness guard that warns when the committed seed no longer
## matches the source.  bootstrap-fpc is the deep-recovery fallback (an fpc build
## of the same source), unexercised and never gated in CI.
KPCOM_SEED        := prebuild/kpcom.kpm
KPCOM_SRC_PAS     := kobold/kpcom.pas
KPCOM_MANIFEST    := prebuild/kpcom.manifest
## The emit inputs, in the order the fingerprint concatenates them.
KPCOM_EMIT_INPUTS := kobold/kpcom.pas kobold/version.inc kobold/stdlib.inc
## A source-based staleness key over the exact emit inputs.  It is a source
## compare, not a byte compare of the module, so a different host toolchain does
## not spuriously flag drift.  kpcom resolves {$I} relative to the source file,
## so the emit runs from the repo root with no staging of version.inc/stdlib.inc.
KPCOM_FINGERPRINT = $(shell cat $(KPCOM_EMIT_INPUTS) 2>/dev/null | sha256sum | cut -d' ' -f1)

## Regenerate the committed seed by the self-host fixed point, then rewrite the
## manifest.  Stage 1 compiles the source with the committed seed as the front
## end; stage 2 compiles it again with stage 1 as the front end; the two modules
## must be byte identical (the fixed point).  Only then does stage 1 become the
## new seed.  Uses the host JIT with KPCOM_KPM overriding the embedded seed, so
## the committed seed drives its own regeneration regardless of what was baked
## into build/skj-kp-jit.  This is the only target that writes prebuild/.
update-kpcom: build/skj-kp-jit
	@set -e; \
	echo "update-kpcom: stage 1 (committed seed compiles the source)"; \
	KPCOM_KPM=$(KPCOM_SEED) build/skj-kp-jit --emit-kpm $(KPCOM_SRC_PAS) > build/kpcom.kpm.s1; \
	echo "update-kpcom: stage 2 (stage 1 compiles the source)"; \
	KPCOM_KPM=build/kpcom.kpm.s1 build/skj-kp-jit --emit-kpm $(KPCOM_SRC_PAS) > build/kpcom.kpm.s2; \
	if ! cmp -s build/kpcom.kpm.s1 build/kpcom.kpm.s2; then \
	    echo "update-kpcom: FAIL self-host fixed point broke (stage 1 != stage 2); seed not written" >&2; \
	    exit 1; \
	fi; \
	cp build/kpcom.kpm.s1 $(KPCOM_SEED); \
	fp='$(KPCOM_FINGERPRINT)'; \
	ks=$$(sha256sum $(KPCOM_SEED) | cut -d' ' -f1); \
	kb=$$(wc -c < $(KPCOM_SEED) | tr -d ' '); \
	{ echo "# regenerated by 'make update-kpcom'; do not edit by hand"; \
	  echo "# The committed kpcom.kpm is the self-host bootstrap seed (stage0): the KPM1 module"; \
	  echo "# (serialized shared IR) that kpcom emits when it compiles its own source,"; \
	  echo "# kobold/kpcom.pas.  The fingerprint is a source-based staleness key over the exact"; \
	  echo "# emit inputs, in order:"; \
	  echo "#   kpcom.pas  version.inc  stdlib.inc"; \
	  echo "# emit-flags are the kpcom arguments that produced kpcom.kpm.  See doc/languages/kobold/kobold.md."; \
	  echo "fingerprint $$fp"; \
	  echo "emit-flags --emit-kpm"; \
	  echo "kpm-sha256 $$ks"; \
	  echo "kpm-bytes $$kb"; } > $(KPCOM_MANIFEST); \
	echo "update-kpcom: refreshed $(KPCOM_SEED) ($$kb bytes, fingerprint $$fp)"

## Can this host regenerate the seed?  update-kpcom runs the x86-64 in-process
## JIT (build/skj-kp-jit), which executes natively only on an x86-64 host, so
## that is the toolchain gate, the analog of Kobold's RV_GCC_OK.
KPJIT_OK := $(shell test "$$(uname -m)" = x86_64 && echo yes)

## Guard the committed seed against its source.  Up to date: pass.  Stale with a
## host that can regenerate: refresh it (the self-host fixed point in
## update-kpcom, which fails if the fixed point breaks, so check-all gates on a
## regenerable seed).  Stale without: warn and use the committed seed as is.
## Mirrors Kobold's check-prebuilt.
check-kpcom:
	@test -f $(KPCOM_SEED) || { echo "check-kpcom: missing $(KPCOM_SEED)" >&2; exit 1; }
	@test -f $(KPCOM_MANIFEST) || { echo "check-kpcom: missing $(KPCOM_MANIFEST)" >&2; exit 1; }
	@rec=$$(sed -n 's/^fingerprint //p' $(KPCOM_MANIFEST)); cur='$(KPCOM_FINGERPRINT)'; \
	if [ "$$rec" = "$$cur" ]; then \
	    echo "check-kpcom: $(KPCOM_SEED) up to date ($$cur)"; \
	elif [ "$(KPJIT_OK)" = yes ]; then \
	    echo "check-kpcom: seed source changed, regenerating $(KPCOM_SEED)"; \
	    $(MAKE) update-kpcom; \
	else \
	    echo "check-kpcom: WARNING $(KPCOM_SEED) is stale (recorded $$rec, current $$cur)" >&2; \
	    echo "check-kpcom: no x86-64 host to regenerate; run 'make update-kpcom' on an x86-64 host and commit prebuild/" >&2; \
	fi

## Deep-recovery fallback: rebuild the seed from source with fpc, a toolchain
## independent of both skjegg and a working Kobold.  It writes build/kpcom.kpm.fpc
## only; update-kpcom stays the sole writer of the committed seed.  To adopt the
## result: cp build/kpcom.kpm.fpc prebuild/kpcom.kpm && make update-kpcom.
## Unexercised and never gated in CI.
FPC ?= fpc
bootstrap-fpc: | build
	@command -v $(FPC) >/dev/null 2>&1 || { echo "bootstrap-fpc: '$(FPC)' required" >&2; exit 1; }
	$(FPC) -Fikobold -FEbuild -FUbuild -O2 -Xs -veiwn kobold/kpcom.pas
	build/kpcom --emit-kpm kobold/kpcom.pas > build/kpcom.kpm.fpc
	@echo "bootstrap-fpc: wrote build/kpcom.kpm.fpc ($$(wc -c < build/kpcom.kpm.fpc | tr -d ' ') bytes, sha256 $$(sha256sum build/kpcom.kpm.fpc | cut -d' ' -f1))"
	@echo "bootstrap-fpc: to adopt, cp build/kpcom.kpm.fpc prebuild/kpcom.kpm && make update-kpcom"

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
	@MODE=jit JIT=build/skj-kp-jit sh tests/run-kp-src-tests.sh

## Native front end emits a module, the x86-64 backend AOT-compiles and links it,
## the program runs; stdout must match the .expected file.
check-kp-aot: build/skj-kpc build/skj-kp build/libkpaot.a
	@MODE=aot FRONTEND=build/skj-kpc \
	    KPAOT=build/skj-kp KPAOTLIB=build/libkpaot.a \
	    KPAOTCC=$(CC) KPAOTAS="$(X64_ASM) -f elf64" \
	    sh tests/run-kp-src-tests.sh

## The RV32 and AArch64 cross runs, under qemu.  JIT: the embedded front end JITs
## for the cross target in-process.  AOT: the host front end emits the module, the
## cross backend compiles and links it, qemu runs it.
check-kp-jit-rv: build/skj-kp-jit-rv
	@MODE=jit JIT=build/skj-kp-jit-rv QEMU="$(QEMU_RV)" sh tests/run-kp-src-tests.sh

check-kp-jit-arm64: build/skj-kp-jit-arm64
	@MODE=jit JIT=build/skj-kp-jit-arm64 QEMU="$(QEMU_A64)" sh tests/run-kp-src-tests.sh

check-kp-aot-rv: build/skj-kpc build/skj-kp-rv build/libkpaot-rv.a
	@MODE=aot FRONTEND=build/skj-kpc \
	    KPAOT=build/skj-kp-rv KPAOTLIB=build/libkpaot-rv.a \
	    KPAOTCC="$(RVJIT_CC)" KPAOTQEMU="$(QEMU_RV)" \
	    sh tests/run-kp-src-tests.sh

check-kp-aot-arm64: build/skj-kpc build/skj-kp-arm64 build/libkpaot-arm64.a
	@MODE=aot FRONTEND=build/skj-kpc \
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
	@KP_SRCDIR=tests/kp-corpus MODE=jit JIT=build/skj-kp-jit sh tests/run-kp-src-tests.sh
	@KP_SRCDIR=tests/kp-corpus MODE=aot FRONTEND=build/skj-kpc \
	    KPAOT=build/skj-kp KPAOTLIB=build/libkpaot.a \
	    KPAOTCC=$(CC) KPAOTAS="$(X64_ASM) -f elf64" \
	    sh tests/run-kp-src-tests.sh

## The cross-arch corpus runs the AOT path only: the host front end and backend do
## the work, qemu runs the small program (a cross JIT would emulate JITing the
## whole seed, far too slow for the whole corpus).
check-kp-corpus-rv: build/skj-kpc build/skj-kp-rv build/libkpaot-rv.a
	@KP_SRCDIR=tests/kp-corpus MODE=aot FRONTEND=build/skj-kpc \
	    KPAOT=build/skj-kp-rv KPAOTLIB=build/libkpaot-rv.a \
	    KPAOTCC="$(RVJIT_CC)" KPAOTQEMU="$(QEMU_RV)" \
	    sh tests/run-kp-src-tests.sh

check-kp-corpus-arm64: build/skj-kpc build/skj-kp-arm64 build/libkpaot-arm64.a
	@KP_SRCDIR=tests/kp-corpus MODE=aot FRONTEND=build/skj-kpc \
	    KPAOT=build/skj-kp-arm64 KPAOTLIB=build/libkpaot-arm64.a \
	    KPAOTCC="$(A64_CC)" KPAOTQEMU="$(QEMU_A64)" \
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
	@KP_SRCDIR=tests/kp-multi MODE=jit JIT=build/skj-kp-jit sh tests/run-kp-multi-tests.sh
	@KP_SRCDIR=tests/kp-multi MODE=aot FRONTEND=build/skj-kpc \
	    KPAOT=build/skj-kp KPAOTLIB=build/libkpaot.a \
	    KPAOTCC=$(CC) KPAOTAS="$(X64_ASM) -f elf64" \
	    sh tests/run-kp-multi-tests.sh

check-kp-multi-rv: build/skj-kpc build/skj-kp-rv build/libkpaot-rv.a
	@KP_SRCDIR=tests/kp-multi MODE=aot FRONTEND=build/skj-kpc \
	    KPAOT=build/skj-kp-rv KPAOTLIB=build/libkpaot-rv.a \
	    KPAOTCC="$(RVJIT_CC)" KPAOTQEMU="$(QEMU_RV)" \
	    sh tests/run-kp-multi-tests.sh

check-kp-multi-arm64: build/skj-kpc build/skj-kp-arm64 build/libkpaot-arm64.a
	@KP_SRCDIR=tests/kp-multi MODE=aot FRONTEND=build/skj-kpc \
	    KPAOT=build/skj-kp-arm64 KPAOTLIB=build/libkpaot-arm64.a \
	    KPAOTCC="$(A64_CC)" KPAOTQEMU="$(QEMU_A64)" \
	    sh tests/run-kp-multi-tests.sh

## ---- the input-dependent corpus (tests/kp-io) ----
## Programs that read stdin or take arguments: a .input file is the program's
## stdin and a .args file its command line (ParamStr/ParamCount), with the
## .expected the reference kprun's output under the same.  This is the one surface
## the other corpora omit (they run with no input), so it exercises the runner's
## stdin/argv plumbing across every tier.  x86 JIT and AOT are in check-all; the
## rv32 and arm64 AOT paths and the freestanding tier run in check-emu-all, so the
## entry argument vector and the read seam are checked on the no-libc tier too.
check-kp-io: build/skj-kp-jit build/skj-kpc build/skj-kp build/libkpaot.a
	@KP_SRCDIR=tests/kp-io MODE=jit JIT=build/skj-kp-jit sh tests/run-kp-src-tests.sh
	@KP_SRCDIR=tests/kp-io MODE=aot FRONTEND=build/skj-kpc \
	    KPAOT=build/skj-kp KPAOTLIB=build/libkpaot.a \
	    KPAOTCC=$(CC) KPAOTAS="$(X64_ASM) -f elf64" \
	    sh tests/run-kp-src-tests.sh

check-kp-io-rv: build/skj-kpc build/skj-kp-rv build/libkpaot-rv.a
	@KP_SRCDIR=tests/kp-io MODE=aot FRONTEND=build/skj-kpc \
	    KPAOT=build/skj-kp-rv KPAOTLIB=build/libkpaot-rv.a \
	    KPAOTCC="$(RVJIT_CC)" KPAOTQEMU="$(QEMU_RV)" \
	    sh tests/run-kp-src-tests.sh

check-kp-io-arm64: build/skj-kpc build/skj-kp-arm64 build/libkpaot-arm64.a
	@KP_SRCDIR=tests/kp-io MODE=aot FRONTEND=build/skj-kpc \
	    KPAOT=build/skj-kp-arm64 KPAOTLIB=build/libkpaot-arm64.a \
	    KPAOTCC="$(A64_CC)" KPAOTQEMU="$(QEMU_A64)" \
	    sh tests/run-kp-src-tests.sh

check-kp-io-free: build/skj-kpc build/skj-kp-rv build/skj-as-rv build/skj-ld-rv \
                  build/kpfree-rv/kp_crt_rv.o build/libkpaot-rv-free.a
	@KP_SRCDIR=tests/kp-io MODE=free FRONTEND=build/skj-kpc \
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
	@KP_SRCDIR=tests/kp-inc MODE=jit JIT=build/skj-kp-jit sh tests/run-kp-src-tests.sh
	@KP_SRCDIR=tests/kp-inc MODE=aot FRONTEND=build/skj-kpc \
	    KPAOT=build/skj-kp KPAOTLIB=build/libkpaot.a \
	    KPAOTCC=$(CC) KPAOTAS="$(X64_ASM) -f elf64" \
	    sh tests/run-kp-src-tests.sh

check-kp-inc-rv: build/skj-kpc build/skj-kp-rv build/libkpaot-rv.a
	@KP_SRCDIR=tests/kp-inc MODE=aot FRONTEND=build/skj-kpc \
	    KPAOT=build/skj-kp-rv KPAOTLIB=build/libkpaot-rv.a \
	    KPAOTCC="$(RVJIT_CC)" KPAOTQEMU="$(QEMU_RV)" \
	    sh tests/run-kp-src-tests.sh

check-kp-inc-arm64: build/skj-kpc build/skj-kp-arm64 build/libkpaot-arm64.a
	@KP_SRCDIR=tests/kp-inc MODE=aot FRONTEND=build/skj-kpc \
	    KPAOT=build/skj-kp-arm64 KPAOTLIB=build/libkpaot-arm64.a \
	    KPAOTCC="$(A64_CC)" KPAOTQEMU="$(QEMU_A64)" \
	    sh tests/run-kp-src-tests.sh

check-kp-inc-free: build/skj-kpc build/skj-kp-rv build/skj-as-rv build/skj-ld-rv \
                   build/kpfree-rv/kp_crt_rv.o build/libkpaot-rv-free.a
	@KP_SRCDIR=tests/kp-inc MODE=free FRONTEND=build/skj-kpc \
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
KPFREE_RV_CFLAGS = $(RV_RT_CFLAGS) -fno-pic -mno-relax -DKP_FREESTANDING -DKP_REAL=double -Ikobold

build/kpfree-rv:
	mkdir -p build/kpfree-rv

build/kpfree-rv/%.o: kobold/%.c $(KP_HDRS_RV) Makefile | build/kpfree-rv
	$(RV_CC) $(KPFREE_RV_CFLAGS) -c -o $@ $<

build/kpfree-rv/soft64.o: runtime/soft64.c Makefile | build/kpfree-rv
	$(RV_CC) $(RV_RT_CFLAGS) -fno-pic -mno-relax -DSOFT64_NO_MULSHIFT -c -o $@ $<

build/kpfree-rv/kp_crt_rv.o: kobold/kp_crt_rv.S build/skj-as-rv | build/kpfree-rv
	./build/skj-as-rv -o $@ $<

# The i64 helpers use GNU pseudo-ops (tail) skj-as-rv does not have; they are
# prebuilt library pieces, so the rv cross assembler builds them (the hosted
# archive assembles the same sources with its cross assembler).
build/kpfree-rv/rv_i64_aot_alias.o: kobold/rv_i64_aot_alias.S | build/kpfree-rv
	$(RV_CC) $(RV_ARCH) -fno-pic -mno-relax -c -o $@ $<

build/kpfree-rv/rv_jit_i64.o: jit/rv_jit_i64.S | build/kpfree-rv
	$(RV_CC) $(RV_ARCH) -fno-pic -mno-relax -c -o $@ $<

# The coroutine context switch uses fsd/fld, so the rv cross assembler builds it
# (like the i64 helpers); co_free.c goes through the generic rule above.
build/kpfree-rv/co_switch_rv.o: runtime/co_switch_rv.S | build/kpfree-rv
	$(RV_CC) $(RV_ARCH) -fno-pic -mno-relax -c -o $@ $<

KPFREE_RV_ARCHIVE := build/kpfree-rv/rt_free.o build/kpfree-rv/io_free.o \
                     build/kpfree-rv/str_core.o build/kpfree-rv/dynarr.o \
                     build/kpfree-rv/map.o build/kpfree-rv/soft64.o \
                     build/kpfree-rv/rv_i64_aot_alias.o build/kpfree-rv/rv_jit_i64.o \
                     build/kpfree-rv/co_free.o build/kpfree-rv/co_switch_rv.o \
                     build/kpfree-rv/math_free.o

build/libkpaot-rv-free.a: $(KPFREE_RV_ARCHIVE) | build/kpfree-rv
	$(AR) rcs $@ $(KPFREE_RV_ARCHIVE)

## check-kp-free: the boris definition-of-done pipeline, from Kobold source to a
## freestanding rv32 ELF, with nothing but the skj toolchain past the front end.
## The host skj-kpc emits the module, skj-kp-rv compiles it, skj-as-rv assembles,
## and skj-ld-rv links the crt, the program, and libkpaot-rv-free.a into a static
## ELF whose only host needs are write/read/exit and the handed-in heap.  Run under
## qemu-riscv32 (which trivially provides that seam); stdout must match .expected.
check-kp-free: build/skj-kpc build/skj-kp-rv build/skj-as-rv build/skj-ld-rv \
               build/kpfree-rv/kp_crt_rv.o build/libkpaot-rv-free.a
	@MODE=free FRONTEND=build/skj-kpc \
	    KPRV=build/skj-kp-rv KPAS=build/skj-as-rv KPLD=build/skj-ld-rv \
	    KPCRT=build/kpfree-rv/kp_crt_rv.o KPLIB=build/libkpaot-rv-free.a \
	    QEMU="$(QEMU_RV)" \
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
	$(A64_CC) $(CFLAGS) -DCC_LP64 -DCC_PSABI -DCC_ARM64 $(SKJ_INC_DEF) -Icc -Icpp -Iir -Ijit -Ibackend -o $@ $(JIT_SRC_A64)

## Run the C suite in-process through skj-jit-arm64 under qemu-aarch64.  The
## arm64 JIT supports _Float16 natively (fcvt), so it excludes only inline asm.
check-jit-arm64: build/skj-jit-arm64
	@JIT="$(CURDIR)/build/skj-jit-arm64" RUNNER="$(QEMU_A64)" \
	 EXCLUDE="cc_t077_inline_asm" sh tests/run-jit-tests.sh

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
build/skj-jit-rv: $(JIT_SRC_RV) cc/cc.h jit/jit_common.h jit/jit_arena.h jit/jit_x86.h jit/emit_rv.h jit/code.h backend/rv_mc.h backend/rv_select.h ir/ir.h Makefile build/version.h | build
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

EMU_SRC   := emu/main.c emu/guest.c emu/elf32.c
EMU_HDR   := emu/guest.h emu/elf32.h
EMU_FLAGS := -DEMU_COLDFIRE=$(EMU_COLDFIRE) -DEMU_RV32=$(EMU_RV32)
ifeq ($(EMU_COLDFIRE),1)
EMU_SRC += emu/coldfire.c emu/cf_user.c
EMU_HDR += emu/coldfire.h
endif
ifeq ($(EMU_RV32),1)
EMU_SRC += emu/rv32.c emu/rv_user.c
EMU_HDR += emu/rv32.h
endif

build/skj-run: $(EMU_SRC) $(EMU_HDR) build/version.h | build
	$(CC) $(CFLAGS) $(EMU_FLAGS) -Iemu -o $@ $(EMU_SRC) -lm

## ColdFire assembler
AS_SRC := as/main.c as/asm_lex.c as/asm_obj.c as/parse.c as/encode.c as/elf.c \
          ir/util.c ir/arena.c

build/skj-as: $(AS_SRC) as/as.h as/asm_lex.h as/asm_obj.h | build
	$(CC) $(CFLAGS) -Ias -Iir -o $@ $(AS_SRC)

## RISC-V RV32 assembler (shares the lexer + object helpers with skj-as)
AS_RV_SRC := as/rv_main.c as/asm_lex.c as/asm_obj.c as/rv_parse.c \
             as/rv_encode.c as/rv_macro.c as/rv_elf.c ir/util.c ir/arena.c

build/skj-as-rv: $(AS_RV_SRC) as/rv.h as/asm_lex.h as/asm_obj.h | build
	$(CC) $(CFLAGS) -Ias -Iir -o $@ $(AS_RV_SRC)

## MIPS I assembler (shares the lexer + object helpers with skj-as)
## Built up in steps; step 2 encodes the instruction set.  See doc/tools/mips.md.
AS_MIPS_SRC := as/mips_main.c as/asm_lex.c as/asm_obj.c as/mips_parse.c \
               as/mips_encode.c as/mips_elf.c as/mips_sched.c ir/util.c ir/arena.c

build/skj-as-mips: $(AS_MIPS_SRC) as/mips.h as/asm_lex.h as/asm_obj.h | build
	$(CC) $(CFLAGS) -Ias -Iir -o $@ $(AS_MIPS_SRC)

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

build/start.o: runtime/start.S | build
	$(M68K_AS) -o $@ $<

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
VERSIONED := $(TOOLS) build/skj-tinc-x86-64 build/skj-sc-x86-64 build/skj-exc build/skj-exc-rv
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

check: build/skj-tinc build/skj-mooc build/skj-pc $(TESTS:tests/%.tc=build/%) $(SCM_TESTS:tests/%.scm=build/%) $(MOO_TESTS:tests/%.moo=build/%) $(MOO_TOY_TESTS:tests/%.moo=build/%) $(PAS_TESTS:tests/%.pas=build/%)
	@sh tests/run-tests.sh "$(QEMU)"

## RISC-V per-test rules: tests/<name>.tc -> build/rv/<name>.s -> .o -> binary
RV_TESTS     := $(TESTS)
RV_SCM_TESTS := $(SCM_TESTS)

build/rv/%.s: tests/%.tc build/skj-tinc-rv | build/rv
	./build/skj-tinc-rv -o $@ $<

build/rv/%.o: build/rv/%.s
	$(RV_AS) -march=rv32im -mabi=ilp32 -o $@ $<

build/rv/start.o: runtime/start_rv.S | build/rv
	$(RV_AS) -march=rv32im -mabi=ilp32 -o $@ $<

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
MIPS_RT_CFLAGS := -march=mips1 -EL -G 0 -std=c99 -O2 -Wall -ffreestanding
MIPS_CC ?= mipsel-none-elf-gcc

build/mips/start_rt.o: runtime/start_mips.S | build/mips
	$(MIPS_AS) -march=mips1 -EL -o $@ $<

## gcc-built runtime objects (o32).  Like the RISC-V build/rv/%.o rule, this
## coexists with build/mips/%.o: build/mips/%.s: make picks the .c rule only
## when no matching .s can be produced (libexc, exc_native, utf8, soft64,
## half have no test source).
build/mips/%.o: runtime/%.c | build/mips
	$(MIPS_CC) $(MIPS_RT_CFLAGS) -c -o $@ $<

build/mips/start_psabi.o: runtime/start_mips_psabi.S | build/mips
	$(MIPS_AS) -march=mips1 -EL -o $@ $<

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

## The x86-64 byte encoder (jit/emit_x86.c) checked byte for byte against
## nasm, the golden-master counterpart of check-rvas / check-mipsas.  Needs
## nasm (already an x86 prerequisite); no IR or backend is involved.
## Named test_* so `make check`'s run-tests.sh skips it (it is a host tool,
## not a qemu-m68k guest), the same guard build/test_f32_oracle relies on.
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
	@./build/test_arm64_oracle $(A64_AS) aarch64-linux-gnu-objcopy

## The RV32 byte encoder golden-master (host-only): every jit/emit_rv.c encoder
## checked byte for byte against riscv64-linux-gnu-as, the counterpart of
## check-arm64-oracle.  Runs on the host, no qemu.
build/test_rv_oracle: tests/rv_oracle.c jit/emit_rv.c jit/code.c jit/emit_rv.h jit/code.h | build
	$(CC) $(CFLAGS) -Ijit -o $@ tests/rv_oracle.c jit/emit_rv.c jit/code.c

check-rv-oracle: build/test_rv_oracle
	@./build/test_rv_oracle $(RV_AS) riscv64-linux-gnu-objcopy

## The guest runtime arena (host-only): lazy low-memory mapping, word-aligned
## bump allocation, and clean exhaustion.  Runs on the host, no qemu.
build/test_jit_arena: tests/jit_arena.c jit/jit_arena.c jit/jit_arena.h | build
	$(CC) $(CFLAGS) -Ijit -o $@ tests/jit_arena.c jit/jit_arena.c

test-jit-arena: build/test_jit_arena
	@./build/test_jit_arena

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
	 ASM="$(X64_ASM) -f elf64 -o" LD="$(X64_LD) -m elf_x86_64 -o" \
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
	 X64_ASM="$(X64_ASM)" X64_LD="$(X64_LD)" QEMU_X64="$(QEMU_X64)" \
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
CF_PAS_BINS := $(PAS_TESTS:tests/%.pas=build/cf/%)

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
## these share their prerequisite sets.  exc_walker_test is the exception: it
## is built by test-exc-walker rather than the suite binary lists, so name it
## directly or bench and bench-accept would skip it (and --accept would drop
## it from the baseline).
BENCH_CF_DEPS := $(CF_TC_BINS) $(CF_SCM_BINS) $(CF_MOO_BINS) $(CF_TOY_BINS) \
                 $(CF_PAS_BINS) build/cf/exc_walker_test
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
## Excelsior on RISC-V (the platform-ABI tier)
##
## skj-exc-rv emits the RISC-V ILP32 convention, so the gcc-built guest
## runtime links against it the way it does on ColdFire.  The runtime
## objects are the same portable C, cross-compiled for rv32: soft64.c
## supplies the 64-bit helpers, since the toolchain ships no rv32 libgcc
## to take them from, and start_rv_psabi.S the entry, syscalls, arena
## and source coroutines under that same convention.
## ---------------------------------------------------------------
RV_CC       ?= riscv64-linux-gnu-gcc
RV_AR       ?= riscv64-linux-gnu-ar
RV_ARCH     := -march=rv32imfd -mabi=ilp32
RV_AS_ARCH  := -march=rv32imfd_zfh -mabi=ilp32
RV_RT_CFLAGS := -std=c99 -O2 -Wall -ffreestanding $(RV_ARCH)

build/rv/%.o: runtime/%.c | build/rv
	$(RV_CC) $(RV_RT_CFLAGS) -c -o $@ $<

build/rv/start_psabi.o: runtime/start_rv_psabi.S | build/rv
	$(RV_AS) $(RV_AS_ARCH) -o $@ $<

## Divide-only soft64 for the libc: start_rv.S already defines the mul and
## shift family, so only the __udivdi3 div family must be added (a full
## soft64.o would collide on the shifts).  A separate object from the shared
## build/rv/soft64.o, which the Excelsior runtime links whole.
build/rv/soft64_div.o: runtime/soft64.c | build/rv
	$(RV_CC) $(RV_RT_CFLAGS) -DSOFT64_NO_MULSHIFT -c -o $@ $<

EXC_RV_RT := build/rv/start_psabi.o build/rv/libexc.o build/rv/exc_native.o \
             build/rv/utf8.o build/rv/soft64.o

## Excelsior end to end on RISC-V, under qemu-riscv32.  Not skj-run: the
## RV32 core in emu/ implements F but not D, and Excelsior's float is a
## double, so the emulator cannot run this tier yet.
check-exc-rv: build/skj-exc-rv build/skj-run $(EXC_RV_RT)
	@BDIR="$(CURDIR)/build/rv/exc" \
	 EXC="$(CURDIR)/build/skj-exc-rv" \
	 AS="$(RV_AS) $(RV_AS_ARCH) -o" \
	 LD="$(RV_LD) -m elf32lriscv -o" \
	 START="$(CURDIR)/build/rv/start_psabi.o" \
	 LIBEXC="$(CURDIR)/build/rv/libexc.o" \
	 BINDING="$(CURDIR)/build/rv/exc_native.o" \
	 UTF8="$(CURDIR)/build/rv/utf8.o build/rv/soft64.o" \
	 sh tests/run-exc-tests.sh "$(QEMU_RV)"

## Excelsior end to end on the emulator: libexc and its native binding
## rebuilt for ColdFire, into their own output directory.
check-exc-emu: build/skj-run build/skj-exc build/skj-as build/skj-ld build/start.o \
               build/cf/libexc.o build/cf/exc_native.o build/cf/utf8.o
	@BDIR="$(CURDIR)/build/cf/exc" \
	 LIBEXC="$(CURDIR)/build/cf/libexc.o" \
	 BINDING="$(CURDIR)/build/cf/exc_native.o" \
	 UTF8="$(CURDIR)/build/cf/utf8.o" \
	 sh tests/run-exc-tests.sh ./build/skj-run

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

## Freeze/thaw walker on the emulator: its harness is cross-compiled C, so
## it needs the ColdFire build like the rest of the runtime.
build/cf/exc_walker_test.o: tests/exc_walker_test.c runtime/libexc.h | build/cf
	$(M68K_CC) $(CF_RT_CFLAGS) -c -o $@ $<

build/cf/exc_walker_test: build/start.o build/cf/exc_walker_test.o \
		build/cf/libexc.o build/cf/utf8.o build/exs_walker_fixture.o build/skj-ld
	./build/skj-ld -o $@ build/start.o build/cf/exc_walker_test.o \
		build/cf/libexc.o build/cf/utf8.o build/exs_walker_fixture.o

test-exc-walker-emu: build/skj-run build/cf/exc_walker_test
	@./build/skj-run ./build/cf/exc_walker_test && echo "PASS: exc_walker_test (skj-run)" \
		|| (echo "FAIL: exc_walker_test (skj-run)"; exit 1)

## Excelsior through the RISC-V backend, codegen only: every test in the
## corpus must compile and assemble.  It cannot be linked or run yet (the
## RV backend passes call arguments on the stack, the RISC-V ILP32 ABI
## passes them in registers, so gcc-built libexc is unreachable), so this
## guards the lowering while that gap stands.  The three samples excluded
## below fail identically on ColdFire: they use features neither backend
## lowers yet.
EXC_RV_SKIP := exs_barrow_chest exs_decimal_lit exs_shape_dialog

check-exc-rv-asm: build/skj-exc-rv | build/rv
	@ok=0; bad=0; \
	for f in tests/exs_*.exs; do \
	    n=$$(basename $$f .exs); \
	    case " $(EXC_RV_SKIP) " in *" $$n "*) continue ;; esac; \
	    case $$n in exs_err_*) continue ;; esac; \
	    if ./build/skj-exc-rv -o build/rv/$$n.s $$f 2>build/rv/$$n.err && \
	       $(RV_AS) -march=rv32imfd_zfh -mabi=ilp32 -o build/rv/$$n.o \
	           build/rv/$$n.s 2>>build/rv/$$n.err; then \
	        ok=$$((ok + 1)); \
	    else \
	        bad=$$((bad + 1)); echo "FAIL $$n"; sed 's/^/    /' build/rv/$$n.err; \
	    fi; \
	done; \
	echo "exc rv codegen: $$ok assembled, $$bad failed"; \
	[ $$bad -eq 0 ]

## Excelsior codegen on the 64-bit backends (compile + assemble every program,
## no run).  This verifies the x86-64 / arm64 claim of full Excelsior parity at
## the level where the risk lives, the front-end-to-backend path, since the
## runtime C is shared and portable.  A run tier waits on a register-ABI crt
## with the arena/continuation/coroutine helpers (see skj-exc-x86-64 above).
## The skip set is EXC_RV_SKIP: those three are front-end limitations, not
## backend ones, so every backend skips them alike.
check-exc-x86-64-asm: build/skj-exc-x86-64 | build/x86_64
	@ok=0; bad=0; \
	for f in tests/exs_*.exs; do \
	    n=$$(basename $$f .exs); \
	    case " $(EXC_RV_SKIP) " in *" $$n "*) continue ;; esac; \
	    case $$n in exs_err_*) continue ;; esac; \
	    if ./build/skj-exc-x86-64 -o build/x86_64/$$n.s $$f 2>build/x86_64/$$n.err && \
	       $(X64_ASM) -f elf64 -o build/x86_64/$$n.o build/x86_64/$$n.s 2>>build/x86_64/$$n.err; then \
	        ok=$$((ok + 1)); \
	    else \
	        bad=$$((bad + 1)); echo "FAIL $$n"; sed 's/^/    /' build/x86_64/$$n.err; \
	    fi; \
	done; \
	echo "exc x86-64 codegen: $$ok assembled, $$bad failed"; \
	[ $$bad -eq 0 ]

check-exc-arm64-asm: build/skj-exc-arm64 | build/arm64
	@ok=0; bad=0; \
	for f in tests/exs_*.exs; do \
	    n=$$(basename $$f .exs); \
	    case " $(EXC_RV_SKIP) " in *" $$n "*) continue ;; esac; \
	    case $$n in exs_err_*) continue ;; esac; \
	    if ./build/skj-exc-arm64 -o build/arm64/$$n.s $$f 2>build/arm64/$$n.err && \
	       $(A64_AS) -o build/arm64/$$n.o build/arm64/$$n.s 2>>build/arm64/$$n.err; then \
	        ok=$$((ok + 1)); \
	    else \
	        bad=$$((bad + 1)); echo "FAIL $$n"; sed 's/^/    /' build/arm64/$$n.err; \
	    fi; \
	done; \
	echo "exc arm64 codegen: $$ok assembled, $$bad failed"; \
	[ $$bad -eq 0 ]

## Every suite the in-tree emulator can run, no qemu involved.
check-emu-all: check-emu check-rv-emu check-exc-emu check-exc-rv-emu \
               test-exc-coro-overflow \
               test-exc-walker-emu test-rv-irq test-rv-run test-guest-mmap \
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

build/san/test_guest_mmap: tests/test_guest_mmap.c emu/guest.c emu/guest.h | build build/san
	$(CC) $(CFLAGS) $(SAN_CFLAGS) -Iemu -o $@ tests/test_guest_mmap.c emu/guest.c -lm

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
               build/san/skj-run build/sandbox/parent.elf \
               build/skj-exc-rv build/skj-as-rv build/skj-ld-rv $(EXC_RV_EMU_RT)
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
	@$(MAKE) --no-print-directory check-exc-rv-emu-san
	@$(MAKE) --no-print-directory test-exc-coro-overflow SKJRUN="$(CURDIR)/build/san/skj-run"
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
## The parse/emit tools take full ASan+UBSan.  skj-jit takes UBSan only (see
## UBSAN_CFLAGS).  ASAN_OPTIONS=detect_leaks=0 because the tools arena-allocate
## and exit without freeing (not a bug here), so LeakSanitizer is off while the
## memory-error and UB checks stay on; -fno-sanitize-recover aborts loudly on a
## real finding, which the suite then reports as a tool failure.

build/san/skj-cpp: cpp/main.c $(CPP_SRC) cpp/cpp.h cpp/internal.h build/version.h | build build/san
	$(CC) $(CFLAGS) $(SAN_CFLAGS) -Icpp -Iir -o $@ cpp/main.c $(CPP_SRC)

build/san/skj-cc: $(CC_SRC) cc/cc.h cpp/cpp.h cpp/internal.h ir/ir.h build/version.h | build build/san
	$(CC) $(CFLAGS) $(SAN_CFLAGS) -DCC_M68K $(SKJ_INC_DEF) -Icc -Icpp -Iir -o $@ $(CC_SRC)

build/san/skj-as: $(AS_SRC) as/as.h as/asm_lex.h as/asm_obj.h build/version.h | build build/san
	$(CC) $(CFLAGS) $(SAN_CFLAGS) -Ias -Iir -o $@ $(AS_SRC)

build/san/skj-ld: $(LD_SRC) ld/ld.h ld/mapfile.h build/version.h | build build/san
	$(CC) $(CFLAGS) $(SAN_CFLAGS) -Ild -Iir -o $@ $(LD_SRC)

# EXC_SRC is defined later in the Makefile than this rule, so a prerequisite of
# $(EXC_SRC) would expand empty at read time and never trigger a rebuild; list
# the sources explicitly here (the recipe's $(EXC_SRC) is fine, deferred).
build/san/skj-exc: excelsior/lex.c excelsior/parse.c excelsior/resolve.c \
                   excelsior/typecheck.c excelsior/lower.c excelsior/main.c \
                   $(IR_SRC) $(BE_CF) excelsior/excelsior.h ir/ir.h build/version.h \
                   | build build/san
	$(CC) $(CFLAGS) $(SAN_CFLAGS) -Iexcelsior -Iir -o $@ $(EXC_SRC)

build/san/skj-as-rv: $(AS_RV_SRC) as/rv.h as/asm_lex.h as/asm_obj.h build/version.h | build build/san
	$(CC) $(CFLAGS) $(SAN_CFLAGS) -Ias -Iir -o $@ $(AS_RV_SRC)

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

build/san/as-fuzz: as/as_fuzz.c $(AS_FUZZ_LIB) as/as.h as/asm_lex.h as/asm_obj.h \
                   tests/fuzz_mutate.h build/version.h | build build/san
	$(CC) $(CFLAGS) $(SAN_CFLAGS) -D_GNU_SOURCE -DASFUZZ_CF -Ias -Iir -Itests -o $@ \
	    as/as_fuzz.c $(AS_FUZZ_LIB)

build/san/as-rv-fuzz: as/as_fuzz.c $(AS_RV_FUZZ_LIB) as/rv.h as/asm_lex.h as/asm_obj.h \
                      tests/fuzz_mutate.h build/version.h | build build/san
	$(CC) $(CFLAGS) $(SAN_CFLAGS) -D_GNU_SOURCE -DASFUZZ_RV -Ias -Iir -Itests -o $@ \
	    as/as_fuzz.c $(AS_RV_FUZZ_LIB)

build/san/as-mips-fuzz: as/as_fuzz.c $(AS_MIPS_FUZZ_LIB) as/mips.h as/asm_lex.h as/asm_obj.h \
                        tests/fuzz_mutate.h build/version.h | build build/san
	$(CC) $(CFLAGS) $(SAN_CFLAGS) -D_GNU_SOURCE -DASFUZZ_MIPS -Ias -Iir -Itests -o $@ \
	    as/as_fuzz.c $(AS_MIPS_FUZZ_LIB)

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

# The Excelsior harness runs the full front end: lex -> parse -> resolve ->
# typecheck -> lower.  EXC front-end sources are defined later than this rule,
# so they are listed explicitly (the deferred-definition hazard).
build/san/fe-exc-fuzz: tests/fe_fuzz.c excelsior/lex.c excelsior/parse.c \
                       excelsior/resolve.c excelsior/typecheck.c excelsior/lower.c \
                       $(IR_SRC) $(BE_CF) excelsior/excelsior.h ir/ir.h \
                       tests/fuzz_mutate.h build/version.h | build build/san
	$(CC) $(CFLAGS) $(SAN_CFLAGS) -D_GNU_SOURCE -DFEFUZZ_EXC -Iexcelsior -Iir -Itests -o $@ \
	    tests/fe_fuzz.c excelsior/lex.c excelsior/parse.c excelsior/resolve.c \
	    excelsior/typecheck.c excelsior/lower.c $(IR_SRC) $(BE_CF)

build/san/skj-jit: $(JIT_SRC) cc/cc.h jit/jit_common.h jit/jit_arena.h jit/jit_x86.h jit/emit_x86.h jit/code.h backend/mc.h backend/x86_select.h ir/ir.h build/version.h | build build/san
	$(CC) $(CFLAGS) $(UBSAN_CFLAGS) -DX86_BITS=64 -DCC_PSABI -DCC_STRUCT_ABI $(SKJ_INC_DEF) -Icc -Icpp -Iir -Ijit -Ibackend -o $@ $(JIT_SRC)

# The remaining front ends (tinc, scheme, moo, Pascal) and the archive tool
# take full ASan+UBSan: pure compile-and-emit, no low mmap to collide with the
# ASan shadow.  Each is probed compile-only by run-compile-san.sh.  SC_SRC,
# MOO_SRC, and PC_SRC are defined later in the Makefile than these rules, so a
# prerequisite of them would expand empty at read time and never trigger a
# rebuild (the same deferred-definition hazard the skj-exc rule above notes);
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

# skj-kp-jit maps executable memory low for the JIT, which collides with the
# ASan shadow on x86-64 (the same reason skj-jit is UBSan-only), so the Kobold
# behavioral corpora run under UBSan only.  kp_co.o stays unsanitized (raw
# stack-switch coroutine assembly); UBSan is per-TU, so linking it in is fine.
build/san/skj-kp-jit: kobold/jit_main.c kobold/frontend.c build/kpcom_kpm.c $(KP_RT) build/kp_co.o kobold/frontend.h $(KP_HDRS) build/version.h Makefile | build build/san
	$(CC) $(KP_CFLAGS) $(UBSAN_CFLAGS) -Ibuild -o $@ kobold/jit_main.c kobold/frontend.c build/kpcom_kpm.c $(KP_RT) build/kp_co.o -lm

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

check-san-exc: build/san/skj-exc build/san/skj-as build/san/skj-ld \
               build/start.o build/libexc.o build/exc_native.o build/utf8.o
	@echo "== check-san: Excelsior (skj-exc + skj-as + skj-ld) =="
	@$(SAN_RUNENV) EXC="$(CURDIR)/build/san/skj-exc" AS="$(CURDIR)/build/san/skj-as -o" \
	   LD="$(CURDIR)/build/san/skj-ld -o" BDIR="$(CURDIR)/build/san/exc-out" \
	   sh tests/run-exc-tests.sh "$(QEMU)"

check-san-rvas: build/san/skj-as-rv build/san/skj-ld-rv build/skj-run
	@echo "== check-san: RV32 assembler/linker (skj-as-rv + skj-ld-rv) =="
	@$(SAN_RUNENV) SKJ="$(CURDIR)/build/san/skj-as-rv" SKJLD="$(CURDIR)/build/san/skj-ld-rv" \
	   RUN="$(CURDIR)/build/skj-run" GAS="$(RV_AS)" GLD="$(RV_LD)" QEMU="$(QEMU_RV)" \
	   sh tests/run-rvas-tests.sh

check-san-kp: build/san/skj-kp
	@echo "== check-san: KPM1 reader (skj-kp) =="
	@$(SAN_RUNENV) KP="$(CURDIR)/build/san/skj-kp" sh tests/run-kp-tests.sh

check-san-jit: build/san/skj-jit
	@echo "== check-san: C via the JIT (skj-jit, UBSan) =="
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
## through the UBSan-only skj-kp-jit, run in JIT mode so the sanitized driver
## both compiles and executes each program.
check-san-kp-corpus: build/san/skj-kp-jit
	@echo "== check-san: Kobold hosted corpus (skj-kp-jit, UBSan) =="
	@$(SAN_RUNENV) KP_SRCDIR=tests/kp-corpus MODE=jit JIT="$(CURDIR)/build/san/skj-kp-jit" \
	   sh tests/run-kp-src-tests.sh

check-san-kp-multi: build/san/skj-kp-jit
	@echo "== check-san: Kobold multi-unit corpus (skj-kp-jit, UBSan) =="
	@$(SAN_RUNENV) KP_SRCDIR=tests/kp-multi MODE=jit JIT="$(CURDIR)/build/san/skj-kp-jit" \
	   sh tests/run-kp-multi-tests.sh

check-san-kp-io: build/san/skj-kp-jit
	@echo "== check-san: Kobold io corpus (skj-kp-jit, UBSan) =="
	@$(SAN_RUNENV) KP_SRCDIR=tests/kp-io MODE=jit JIT="$(CURDIR)/build/san/skj-kp-jit" \
	   sh tests/run-kp-src-tests.sh

check-san-kp-inc: build/san/skj-kp-jit
	@echo "== check-san: Kobold include corpus (skj-kp-jit, UBSan) =="
	@$(SAN_RUNENV) KP_SRCDIR=tests/kp-inc MODE=jit JIT="$(CURDIR)/build/san/skj-kp-jit" \
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

test-fe-exc-fuzz: build/san/fe-exc-fuzz
	@echo "== Excelsior parser fuzz ($(FE_FUZZ_ITERS) iterations, rng $(FE_FUZZ_RNG)) =="
	@$(SAN_RUNENV) ./build/san/fe-exc-fuzz --mutate $(FE_FUZZ_ITERS) --rng $(FE_FUZZ_RNG) \
	   $(wildcard tests/exs_*.exs) >/dev/null 2>build/fe-exc-fuzz.log \
	   || { echo "fe-exc-fuzz: FAILED"; tail -40 build/fe-exc-fuzz.log; exit 1; }
	@tail -1 build/fe-exc-fuzz.log

## The pure compile-and-emit tools under the sanitizers: the preprocessor, the
## C compiler, every IR front end (tinc, scheme, moo, Pascal, Excelsior), the
## assembler and linker, and the archive tool.  skj-ld is exercised by the
## Excelsior slice (the cc slice links with the system ld).  This is the CI
## slice: self-contained, no RISC-V reference assembler and no cross checkouts.
check-san-tools: check-san-cpp check-san-cc check-san-exc \
                 check-san-tinc check-san-scheme check-san-moo check-san-pc \
                 check-san-ar
	@echo "check-san-tools: preprocessor, compilers, front ends, assembler, linker, and ar clean under the sanitizers."

## The Kobold behavioral corpora through the UBSan-only driver.
check-san-kobold: check-san-kp check-san-kp-corpus check-san-kp-multi \
                  check-san-kp-io check-san-kp-inc
	@echo "check-san-kobold: KPM1 reader and the Kobold corpora clean under UBSan."

## Every sanitized slice.
check-san: check-san-tools check-san-rvas check-san-jit check-san-kobold \
           test-kpm-fuzz test-cpp-fuzz \
           test-as-fuzz test-as-rv-fuzz test-as-mips-fuzz \
           test-fe-tinc-fuzz test-fe-pascal-fuzz test-fe-moo-fuzz \
           test-fe-cc-fuzz test-fe-exc-fuzz
	@echo "All check-san suites passed under the sanitizers."

.PHONY: check-san check-san-tools check-san-kobold \
        check-san-cpp check-san-cc check-san-exc check-san-rvas check-san-kp \
        check-san-jit check-san-tinc check-san-scheme check-san-moo \
        check-san-pc check-san-ar check-san-kp-corpus check-san-kp-multi \
        check-san-kp-io check-san-kp-inc test-kpm-fuzz test-cpp-fuzz \
        test-as-fuzz test-as-rv-fuzz test-as-mips-fuzz \
        test-fe-tinc-fuzz test-fe-pascal-fuzz test-fe-moo-fuzz \
        test-fe-cc-fuzz test-fe-exc-fuzz

## arm64 per-test rules: tests/<name>.tc -> build/arm64/<name>.s -> .o -> binary
A64_TESTS := $(TESTS)
A64_SCM_TESTS := $(SCM_TESTS)

build/arm64/%.s: tests/%.tc build/skj-tinc-arm64 | build/arm64
	./build/skj-tinc-arm64 -o $@ $<

build/arm64/scm_%.s: tests/scm_%.scm build/skj-sc-arm64 | build/arm64
	./build/skj-sc-arm64 -o $@ $<

build/arm64/%.o: build/arm64/%.s
	$(A64_AS) -o $@ $<

build/arm64/start.o: runtime/start_arm64.S | build/arm64
	$(A64_AS) -o $@ $<

build/arm64/start_aapcs.o: runtime/start_arm64_aapcs.S | build/arm64
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

build/x86_64/%.o: build/x86_64/%.s
	$(X64_ASM) -f elf64 -o $@ $<

build/x86_64/start.o: runtime/start_x86_64.asm | build/x86_64
	$(X64_ASM) -f elf64 -o $@ $<

build/x86_64/start_sysv.o: runtime/start_x86_64_sysv.asm | build/x86_64
	$(X64_ASM) -f elf64 -o $@ $<

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
	$(CC) $(CFLAGS) -Ischeme -Iir -o $@ $(SC_SRC_A64)

SC_SRC_X64 := scheme/lex.c scheme/parse.c scheme/print.c scheme/main.c scheme/gc.c \
              scheme/lower.c $(IR_SRC) $(BE_X64)

build/skj-sc-x86-64: $(SC_SRC_X64) scheme/scheme.h scheme/gc.h ir/ir.h | build
	$(CC) $(CFLAGS) -DX86_BITS=64 -Ischeme -Iir -o $@ $(SC_SRC_X64)

## TinScheme codegen tests (.scm -> .s via skj-sc)
build/scm_%.s: tests/scm_%.scm build/skj-sc | build
	./build/skj-sc -o $@ $<

## TinScheme RISC-V codegen tests
build/rv/scm_%.s: tests/scm_%.scm build/skj-sc-rv | build/rv
	./build/skj-sc-rv -o $@ $<

## TinScheme GC test (host-only)
build/test_gc: scheme/test_gc.c scheme/gc.c scheme/gc.h | build
	$(CC) $(CFLAGS) -Ischeme -o $@ scheme/test_gc.c scheme/gc.c

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

## The guest-32 libexc build (host-only).  Compiles the portable Excelsior
## runtime as LP64 host code over the 32-bit guest object model (the
## EXC_GUEST32 mode the arm64 JIT's Excelsior tier uses) and round-trips the
## value helpers, spawn/image seeding, dispatch lookup, and freeze/thaw
## against a low (MAP_32BIT) arena.  No cross toolchain, no qemu.
build/test_libexc_guest32: tests/libexc_guest32.c runtime/libexc.c runtime/utf8.c \
                           runtime/libexc.h runtime/utf8.h | build
	$(CC) $(CFLAGS) -D_GNU_SOURCE -DEXC_GUEST32 -Iruntime -o $@ \
	    tests/libexc_guest32.c runtime/libexc.c runtime/utf8.c

test-libexc-guest32: build/test_libexc_guest32
	./build/test_libexc_guest32

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
build/test_guest_mmap: tests/test_guest_mmap.c emu/guest.c emu/guest.h | build
	$(CC) $(CFLAGS) -Iemu -o $@ tests/test_guest_mmap.c emu/guest.c -lm

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
RV32_INC  := -Iemu -Itests/rv32
RV32_CORE := emu/rv32.c tests/rv32/machine.c
RV32_LOAD := emu/elf32.c emu/guest.c

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
EXC_SRC := excelsior/lex.c excelsior/parse.c excelsior/resolve.c \
           excelsior/typecheck.c excelsior/lower.c excelsior/main.c \
           $(IR_SRC) $(BE_CF)

EXC_FE     := excelsior/lex.c excelsior/parse.c excelsior/resolve.c \
              excelsior/typecheck.c excelsior/lower.c excelsior/main.c
EXC_SRC_RV := $(EXC_FE) $(IR_SRC) $(BE_RV)
EXC_SRC_MIPS := $(EXC_FE) $(IR_SRC) $(BE_MIPS)
EXC_SRC_X64 := $(EXC_FE) $(IR_SRC) $(BE_X64_SEL)
EXC_SRC_A64 := $(EXC_FE) $(IR_SRC) $(BE_A64_SEL)

build/skj-exc: $(EXC_SRC) excelsior/excelsior.h ir/ir.h | build
	$(CC) $(CFLAGS) -Iexcelsior -Iir -o $@ $(EXC_SRC)

## Excelsior through the RISC-V backend.  The front end is
## backend-agnostic, so this is the same sources with BE_RV swapped in.
## Built -DCC_PSABI: on RISC-V the toolkit's stack convention cannot
## reach a gcc-built runtime (m68k's C ABI is stack-based, RISC-V's is
## not), so this tool emits the platform ILP32 convention instead.
build/skj-exc-rv: $(EXC_SRC_RV) excelsior/excelsior.h ir/ir.h | build
	$(CC) $(CFLAGS) -DCC_PSABI -Iexcelsior -Iir -o $@ $(EXC_SRC_RV)

## Excelsior through the MIPS backend, o32 psABI (the counterpart of
## skj-exc-rv): emits the o32 convention so a gcc-built guest runtime links.
build/skj-exc-mips: $(EXC_SRC_MIPS) excelsior/excelsior.h ir/ir.h | build
	$(CC) $(CFLAGS) -DCC_PSABI -Iexcelsior -Iir -o $@ $(EXC_SRC_MIPS)

## Excelsior through the x86-64 and AArch64 backends, emitting the platform
## register ABI (SysV / AAPCS64) so the compiler matches a gcc-built runtime,
## the same flags skj-cc-x86-64 / skj-cc-arm64 use.  These drive the
## codegen-only tiers (check-exc-x86-64-asm / -arm64-asm).  The static AOT link
## has no end-to-end tier on these two: the backends emit an ILP32 address model
## (4-byte pointers and struct fields) and a statically linked LP64 runtime
## disagrees on the width of every shared struct field, and the ILP32 host ABIs
## that would bridge it (x86 x32, arm64 ilp32) are rejected by qemu-user.  The
## in-process JIT bridges it a different way (skj-exc-jit-arm64 below): it maps
## the guest arena below 4GB and links libexc built EXC_GUEST32, an LP64 host
## build that holds every shared struct at guest width.  See excelsior/status.md
## (Roadmap 2).
build/skj-exc-x86-64: $(EXC_SRC_X64) excelsior/excelsior.h ir/ir.h backend/mc.h backend/mc_text.h backend/x86_select.h | build
	$(CC) $(CFLAGS) -DX86_BITS=64 -DCC_LP64 -DCC_PSABI -DCC_STRUCT_ABI -Iexcelsior -Iir -Ibackend -o $@ $(EXC_SRC_X64)

build/skj-exc-arm64: $(EXC_SRC_A64) excelsior/excelsior.h ir/ir.h backend/arm64_mc.h backend/arm64_select.h backend/arm64_mc_text.h | build
	$(CC) $(CFLAGS) -DCC_LP64 -DCC_PSABI -DCC_ARM64 -Iexcelsior -Iir -o $@ $(EXC_SRC_A64)

## skj-exc-jit-arm64: the in-process Excelsior JIT.  The exc front end lowers to
## IR, the jit/ library JITs it to AArch64, and the entry verb runs in-process
## over libexc built EXC_GUEST32 (the guest-32 host runtime).  The driver is
## non-PIE (so host libexc's __exc_self and code sit below 4GB, the guest's
## 32-bit reach) and -rdynamic (so the guest's runtime imports resolve by name
## through dlsym).  Cross-built with the arm64 toolchain, run under qemu-aarch64.
EXC_FE_NOMAIN := excelsior/lex.c excelsior/parse.c excelsior/resolve.c \
                 excelsior/typecheck.c excelsior/lower.c
EXC_JIT_A64 := jit/exc_main.c jit/jit_common.c jit/jit_arena.c jit/jit_arm64.c \
               jit/emit_arm64.c jit/code.c jit/call_guest_arm64.S \
               jit/exc_coro_arm64.S \
               backend/arm64_select.c backend/regalloc_arm64.c \
               $(EXC_FE_NOMAIN) $(IR_SRC) runtime/libexc.c runtime/utf8.c
build/skj-exc-jit-arm64: $(EXC_JIT_A64) excelsior/excelsior.h ir/ir.h jit/jit_common.h jit/jit_arena.h jit/emit_arm64.h jit/code.h backend/arm64_mc.h backend/arm64_select.h runtime/libexc.h runtime/utf8.h Makefile build/version.h | build
	$(A64_CC) $(CFLAGS) -DCC_LP64 -DCC_PSABI -DCC_ARM64 -DEXC_GUEST32 -no-pie -rdynamic \
	    -Iexcelsior -Iir -Ijit -Ibackend -Iruntime -o $@ $(EXC_JIT_A64) -ldl

## Run the Excelsior end-to-end suite in-process through skj-exc-jit-arm64 under
## qemu-aarch64, comparing exit codes and stdout.  Source coroutines run through
## jit/exc_coro_arm64.S, so the whole suite is in.
check-exc-jit-arm64: build/skj-exc-jit-arm64
	@JIT="$(CURDIR)/build/skj-exc-jit-arm64" RUNNER="$(QEMU_A64)" \
	 sh tests/run-exc-jit-tests.sh

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

## Pascal runtime (cross-compiled C)
build/pascal_rt.o: runtime/pascal_rt.c | build
	$(M68K_CC) -std=c99 -O2 -Wall -ffreestanding -c -o $@ $<

## Excelsior guest runtime and its native host binding (host-abi.md:
## libexc is layer 1, portable; exc_native is the layer-2 binding the
## test host uses; a VM-host binding replaces only the latter)
build/libexc.o: runtime/libexc.c runtime/libexc.h runtime/utf8.h | build
	$(M68K_CC) -std=c99 -O2 -Wall -ffreestanding -c -o $@ $<

build/exc_native.o: runtime/exc_native.c runtime/libexc.h | build
	$(M68K_CC) -std=c99 -O2 -Wall -ffreestanding -c -o $@ $<

## Excelsior on smolmoo (host-abi.md D8): the first VM-host binding.
## smolmoo is a RISC-V RV32 machine (rv32.c, vm_rv.ld), so the verb ELF
## is built through the RV32 psABI toolchain.  The C runtime (libexc, the
## binding, utf8, soft64) is gcc-built -fno-pic -mno-relax, exactly like
## check-exc-rv-skj, so skj-ld-rv can resolve it without a GOT or linker
## relaxation.  The binding is crt-less: exc_smolmoo.c supplies _start and
## the ecall hypercall stubs, the coroutine shim (smolmoo_rt_rv.S) and the
## verb are assembled by skj-as-rv, and the whole program is linked by
## skj-ld-rv over runtime/smolmoo_rv.ld (code at 0x400).  runtime/soft64.c
## supplies the 64-bit helpers the toolchain's absent rv32 libgcc cannot.
## Link-only smoke in this tree: the ELF runs under the smolmoo server,
## whose ecall hypercalls have no Linux meaning, not qemu.  make
## smolmoo-demo proves the pipeline; M0 in doc/languages/smolmoo-v1.md runs it on a
## real server.
build/smolmoo:
	mkdir -p build/smolmoo

build/smolmoo/%.o: runtime/%.c runtime/libexc.h | build/smolmoo
	$(RV_CC) -std=c99 $(RV_SKJ_RT_CFLAGS) -c -o $@ $<

build/smolmoo/smolmoo_rt_rv.o: runtime/smolmoo_rt_rv.S build/skj-as-rv | build/smolmoo
	./build/skj-as-rv -o $@ $<

build/smolmoo/demo.s: build/skj-exc-rv tests/exs_source_yield.exs | build/smolmoo
	./build/skj-exc-rv -o $@ tests/exs_source_yield.exs

build/smolmoo/demo.o: build/smolmoo/demo.s build/skj-as-rv
	./build/skj-as-rv -o $@ $<

build/smolmoo/demo.elf: build/smolmoo/demo.o build/smolmoo/libexc.o \
		build/smolmoo/exc_smolmoo.o build/smolmoo/smolmoo_rt_rv.o \
		build/smolmoo/soft64.o build/smolmoo/utf8.o \
		runtime/smolmoo_rv.ld build/skj-ld-rv
	./build/skj-ld-rv -T runtime/smolmoo_rv.ld -o $@ \
		build/smolmoo/demo.o build/smolmoo/libexc.o \
		build/smolmoo/exc_smolmoo.o build/smolmoo/smolmoo_rt_rv.o \
		build/smolmoo/soft64.o build/smolmoo/utf8.o

smolmoo-demo: build/smolmoo/demo.elf

.PHONY: smolmoo-demo

## Freeze/thaw walker test (host-abi.md D7): the compiled fixture
## class's descriptor field table driven by a C harness that is its own
## binding (an in-memory property store), self-checking under qemu.
build/exs_walker_fixture.o: tests/exs_walker_fixture.exs build/skj-exc build/skj-as
	./build/skj-exc -o build/exs_walker_fixture.s tests/exs_walker_fixture.exs
	./build/skj-as -o $@ build/exs_walker_fixture.s

build/exc_walker_test.o: tests/exc_walker_test.c runtime/libexc.h | build
	$(M68K_CC) -std=c99 -O2 -Wall -ffreestanding -c -o $@ $<

build/exc_walker_test: build/start.o build/exc_walker_test.o build/libexc.o \
		build/utf8.o build/exs_walker_fixture.o build/skj-ld
	./build/skj-ld -o $@ build/start.o build/exc_walker_test.o \
		build/libexc.o build/utf8.o build/exs_walker_fixture.o

test-exc-walker: build/exc_walker_test
	@echo "Running freeze/thaw walker test under qemu-m68k..."
	@$(QEMU) ./build/exc_walker_test && echo "PASS: exc_walker_test" \
		|| (echo "FAIL: exc_walker_test"; exit 1)

.PHONY: test-exc-walker

## Vendored UTF-8 decoder (freestanding), for R7 code-point string ops
build/utf8.o: runtime/utf8.c runtime/utf8.h | build
	$(M68K_CC) -std=c99 -O2 -Wall -ffreestanding -c -o $@ $<

## Pascal codegen tests (.pas -> .s via skj-pc)
build/pascal_%.s: tests/pascal_%.pas build/skj-pc | build
	./build/skj-pc -o $@ $<

## Pascal test binaries need runtime objects
PAS_BINS := $(PAS_TESTS:tests/%.pas=build/%)
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

## I64 integration test (IR builder → arm64 asm → qemu-aarch64)
I64_TEST_A64_SRC := tests/test_i64.c $(IR_SRC) $(BE_A64)

build/test_i64_arm64: $(I64_TEST_A64_SRC) ir/ir.h | build
	$(CC) $(CFLAGS) -Iir -o $@ $(I64_TEST_A64_SRC)

build/arm64/i64_test.s: build/test_i64_arm64 | build/arm64
	./build/test_i64_arm64 -o $@

build/arm64/i64_test.o: build/arm64/i64_test.s
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

build/x86_64/i64_test.o: build/x86_64/i64_test.s
	$(X64_ASM) -f elf64 -o $@ $<

build/x86_64/i64_test: build/x86_64/i64_test.o build/x86_64/start.o
	$(X64_LD) -m elf_x86_64 -o $@ build/x86_64/start.o $<

test-i64-x86-64: build/x86_64/i64_test
	@echo "Running I64 test under qemu-x86_64..."
	@$(QEMU_X64) ./build/x86_64/i64_test && echo "PASS: i64_test (x86-64)" || (echo "FAIL: i64_test (x86-64)"; exit 1)

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
	$(CC) $(CFLAGS) -Iir -o $@ $(OPS_TEST_A64_SRC)

build/arm64/ops_test.s: build/test_ops_arm64 | build/arm64
	./build/test_ops_arm64 -o $@

build/arm64/ops_test.o: build/arm64/ops_test.s
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

build/x86_64/ops_test.o: build/x86_64/ops_test.s
	$(X64_ASM) -f elf64 -o $@ $<

build/x86_64/ops_test: build/x86_64/ops_test.o build/x86_64/start.o
	$(X64_LD) -m elf_x86_64 -o $@ build/x86_64/start.o $<

test-ops-x86-64: build/x86_64/ops_test
	@echo "Running unsigned ops test under qemu-x86_64..."
	@$(QEMU_X64) ./build/x86_64/ops_test && echo "PASS: ops_test (x86-64)" || (echo "FAIL: ops_test (x86-64)"; exit 1)


## FPU integration test (IR builder → arm64 asm → qemu-aarch64).
## _Float16 FLH/FSH are native fcvt on AArch64, so no half.o helper is linked.
FPU_TEST_A64_SRC := tests/test_fpu.c $(IR_SRC) $(BE_A64)

build/test_fpu_arm64: $(FPU_TEST_A64_SRC) ir/ir.h | build
	$(CC) $(CFLAGS) -Iir -o $@ $(FPU_TEST_A64_SRC)

build/arm64/fpu_test.s: build/test_fpu_arm64 | build/arm64
	./build/test_fpu_arm64 -o $@

build/arm64/fpu_test.o: build/arm64/fpu_test.s
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

build/x86_64/fpu_test.o: build/x86_64/fpu_test.s
	$(X64_ASM) -f elf64 -o $@ $<

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
build/x86_64/f32_test.o: build/x86_64/f32_test.s
	$(X64_ASM) -f elf64 -o $@ $<
build/x86_64/f32_test: build/x86_64/f32_test.o build/x86_64/start.o
	$(X64_LD) -m elf_x86_64 -o $@ build/x86_64/start.o $<
test-f32-x86-64: build/x86_64/f32_test
	@echo "Running F32 test under qemu-x86_64..."
	@$(QEMU_X64) ./build/x86_64/f32_test && echo "PASS: f32_test (x86-64)" || (echo "FAIL: f32_test (x86-64)"; exit 1)

F32_TEST_A64_SRC := tests/test_f32.c $(IR_SRC) $(BE_A64)
build/test_f32_arm64: $(F32_TEST_A64_SRC) ir/ir.h | build
	$(CC) $(CFLAGS) -Iir -o $@ $(F32_TEST_A64_SRC)
build/arm64/f32_test.s: build/test_f32_arm64 | build/arm64
	./build/test_f32_arm64 -o $@
build/arm64/f32_test.o: build/arm64/f32_test.s
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
	 ASM="$(X64_ASM) -f elf64 -o" \
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

## The Excelsior psABI runtime for the skj-toolchain variant: the C parts are
## still gcc-built (skj-cc-rv uses the backend's stack convention, not the
## register psABI skj-exc-rv and the runtime share), but -fno-pic -mno-relax
## keeps them to the relocations skj-ld-rv implements (no GOT, no linker
## relaxation; the SET*/SUB* pairs stay in the discarded .eh_frame).  The crt
## is assembled by skj-as-rv.
RV_SKJ_RT_CFLAGS := $(RV_RT_CFLAGS) -fno-pic -mno-relax

build/rv-skj/%.o: runtime/%.c build/skj-as-rv | build/rv-skj
	$(RV_CC) $(RV_SKJ_RT_CFLAGS) -c -o $@ $<

build/rv-skj/start_psabi.o: runtime/start_rv_psabi.S build/skj-as-rv | build/rv-skj
	./build/skj-as-rv -o $@ $<

EXC_RV_SKJ_RT := build/rv-skj/start_psabi.o build/rv-skj/libexc.o \
                 build/rv-skj/exc_native.o build/rv-skj/utf8.o \
                 build/rv-skj/soft64.o

## Excelsior end to end through the in-tree RV32 toolchain: skj-exc-rv output
## and the psABI crt assembled by skj-as-rv, the whole program (crt + libexc +
## binding + softfloat + verb) linked by skj-ld-rv, run under qemu-riscv32.
check-exc-rv-skj: build/skj-exc-rv build/skj-as-rv build/skj-ld-rv $(EXC_RV_SKJ_RT)
	@BDIR="$(CURDIR)/build/rv-skj/exc" \
	 EXC="$(CURDIR)/build/skj-exc-rv" \
	 AS="$(CURDIR)/build/skj-as-rv -o" \
	 LD="$(CURDIR)/build/skj-ld-rv -o" \
	 START="$(CURDIR)/build/rv-skj/start_psabi.o" \
	 LIBEXC="$(CURDIR)/build/rv-skj/libexc.o" \
	 BINDING="$(CURDIR)/build/rv-skj/exc_native.o" \
	 UTF8="$(CURDIR)/build/rv-skj/utf8.o $(CURDIR)/build/rv-skj/soft64.o" \
	 sh tests/run-exc-tests.sh "$(QEMU_RV)"

## The soft-ctx psABI crt for the emulator.  skj-run's RV32 core has F but
## not D, so the source-coroutine context switch (save_ctx/restore_ctx in
## start_rv_psabi.S) cannot use fsd/fld.  This variant is the mechanical
## fsd->fsw / fld->flw rewrite of that crt; those two macros hold the file's
## only such instructions.  It is correct on the F-only core because skj-run
## never runs double-float code (that tier is qemu's), so a coroutine's saved
## float registers only ever carry 32-bit values here.  A coroutine's private
## stack comes from the guarded gm_mmap region (start_rv_psabi.S installs the
## provider), so a stack overrun faults on the guard instead of the arena.
build/rv-skj/start_psabi_soft.S: runtime/start_rv_psabi.S | build/rv-skj
	sed -E 's/\bfsd\b/fsw/g; s/\bfld\b/flw/g' $< > $@

build/rv-skj/start_psabi_soft.o: build/rv-skj/start_psabi_soft.S build/skj-as-rv
	./build/skj-as-rv -o $@ $<

EXC_RV_EMU_RT := build/rv-skj/start_psabi_soft.o build/rv-skj/libexc.o \
                 build/rv-skj/exc_native.o build/rv-skj/utf8.o \
                 build/rv-skj/soft64.o

## Excelsior source coroutines on the emulator (skj-run), the tier the guarded
## coroutine stack protects.  Built with the soft-ctx crt so the F-only core
## can run the context switch, linked by skj-ld-rv, run on skj-run.  Only the
## double-free coroutine tests: the RV32 core has no D, so the wider suite is
## qemu's (check-exc-rv).
## The coroutine tier: the two source samples plus the V.F stress tests (deep
## wrapper nesting and many simultaneously live coroutines).  All four are the
## soft-ctx save_ctx / restore_ctx path on the F-only core, so skj-run runs
## them; the deep and many tests also ride the portable tiers (they have
## .expected files, so check-exc / check-exc-rv enumerate them).
EXC_RV_EMU_TESTS := exs_source_yield exs_source_cursor exs_coro_deep exs_coro_many

check-exc-rv-emu: build/skj-exc-rv build/skj-as-rv build/skj-ld-rv build/skj-run \
                  $(EXC_RV_EMU_RT)
	@BDIR="$(CURDIR)/build/rv-skj/exc-emu" \
	 EXC="$(CURDIR)/build/skj-exc-rv" \
	 AS="$(CURDIR)/build/skj-as-rv -o" \
	 LD="$(CURDIR)/build/skj-ld-rv -o" \
	 START="$(CURDIR)/build/rv-skj/start_psabi_soft.o" \
	 LIBEXC="$(CURDIR)/build/rv-skj/libexc.o" \
	 BINDING="$(CURDIR)/build/rv-skj/exc_native.o" \
	 UTF8="$(CURDIR)/build/rv-skj/utf8.o $(CURDIR)/build/rv-skj/soft64.o" \
	 ONLY="$(EXC_RV_EMU_TESTS)" \
	 sh tests/run-exc-tests.sh "$(CURDIR)/build/skj-run"

## V.F: the same coroutine tier under the sanitized skj-run (ASan+UBSan), so
## the hand-written context switch and the guarded private stacks are driven
## with the host emulator instrumented, not only by qemu.  Reuses the already
## built cross objects (a guest ELF is unaffected by host sanitizers).
check-exc-rv-emu-san: build/skj-exc-rv build/skj-as-rv build/skj-ld-rv \
                      build/san/skj-run $(EXC_RV_EMU_RT)
	@BDIR="$(CURDIR)/build/rv-skj/exc-emu-san" \
	 EXC="$(CURDIR)/build/skj-exc-rv" \
	 AS="$(CURDIR)/build/skj-as-rv -o" \
	 LD="$(CURDIR)/build/skj-ld-rv -o" \
	 START="$(CURDIR)/build/rv-skj/start_psabi_soft.o" \
	 LIBEXC="$(CURDIR)/build/rv-skj/libexc.o" \
	 BINDING="$(CURDIR)/build/rv-skj/exc_native.o" \
	 UTF8="$(CURDIR)/build/rv-skj/utf8.o $(CURDIR)/build/rv-skj/soft64.o" \
	 ONLY="$(EXC_RV_EMU_TESTS)" \
	 sh tests/run-exc-tests.sh "$(CURDIR)/build/san/skj-run"

## V.F: a coroutine stack overrun must fault on the guard page, not corrupt the
## arena.  exs_coro_overflow recurses unboundedly inside a source body; on
## skj-run that walks the private 4 KB stack into the unmapped guard and faults
## (exit 139, SIGSEGV, "stack overflow" on stderr, no stdout).  It is skj-run
## only (qemu falls back to the arena with no guard) and expects a non-zero
## exit, so it is driven here, not through the enumerated exc runner.  SKJRUN
## overrides the runner, so check-emu-san runs it under the sanitized build too.
test-exc-coro-overflow: build/skj-exc-rv build/skj-as-rv build/skj-ld-rv \
                        build/skj-run $(EXC_RV_EMU_RT)
	@BDIR="$(CURDIR)/build/rv-skj/exc-emu"; mkdir -p "$$BDIR"; \
	t=exs_coro_overflow; \
	./build/skj-exc-rv -o "$$BDIR/$$t.s" tests/$$t.exs 2>"$$BDIR/$$t.err" && \
	./build/skj-as-rv -o "$$BDIR/$$t.o" "$$BDIR/$$t.s" 2>>"$$BDIR/$$t.err" && \
	./build/skj-ld-rv -o "$$BDIR/$$t" $(EXC_RV_EMU_RT) "$$BDIR/$$t.o" 2>>"$$BDIR/$$t.err" || \
	    { echo "FAIL: $$t did not build"; cat "$$BDIR/$$t.err"; exit 1; }; \
	RUN="$${SKJRUN:-$(CURDIR)/build/skj-run}"; \
	"$$RUN" "$$BDIR/$$t" >"$$BDIR/$$t.out" 2>"$$BDIR/$$t.stderr"; rc=$$?; \
	if [ "$$rc" -eq 139 ] && grep -q "stack overflow" "$$BDIR/$$t.stderr" && \
	   [ ! -s "$$BDIR/$$t.out" ]; then \
	    echo "PASS: coroutine stack overrun hits the guard page (exit 139)"; \
	else \
	    echo "FAIL: expected exit 139 with 'stack overflow' and no stdout, got exit $$rc"; \
	    cat "$$BDIR/$$t.stderr"; exit 1; \
	fi

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

build/xlang-sandbox/kchild.kpm: tests/xlang-sandbox/kchild.pas build/skj-kpc | build/xlang-sandbox
	./build/skj-kpc --emit-kpm $< > $@

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
LIBC_SRCS := string ctype stdlib printf stdio assert malloc sbrk errno \
             strerror rand strtod scanf coro threads
LIBC_SRC_FILES := $(addprefix libc/,$(addsuffix .c,$(LIBC_SRCS)))
LIBC_HDRS := $(wildcard include/*.h) libc/fmt.h

## printf's floating conversions (f F e E g G a A) are a compile-time option
## (SKJ_LIBC_FLOAT in libc/printf.c): defined here, the shipped libc.a formats
## floats; omitted, it links an integer-only printf with no double arithmetic.
## On everywhere today.  To ship a float-free libc for a target that cannot run
## double (for example the RV32 skj-run core, which has no D extension), clear
## that target's variable below.
LIBC_CFLAGS_CF   := -DSKJ_LIBC_FLOAT
LIBC_CFLAGS_RV   := -DSKJ_LIBC_FLOAT
LIBC_CFLAGS_MIPS := -DSKJ_LIBC_FLOAT

build/lib/coldfire/libc.a: build/skj-cc build/skj-as build/skj-ar \
                           $(LIBC_SRC_FILES) $(LIBC_HDRS) build/co_switch_cf.o | build
	@mkdir -p build/lib/coldfire/obj
	@for s in $(LIBC_SRCS); do \
	    ./build/skj-cc $(LIBC_CFLAGS_CF) -o build/lib/coldfire/obj/$$s.s libc/$$s.c && \
	    ./build/skj-as -o build/lib/coldfire/obj/$$s.o build/lib/coldfire/obj/$$s.s \
	    || exit 1; \
	done
	@rm -f $@
	@./build/skj-ar rcs $@ $(addprefix build/lib/coldfire/obj/,$(addsuffix .o,$(LIBC_SRCS))) build/co_switch_cf.o

build/lib/riscv/libc.a: build/skj-cc-rv build/skj-as-rv build/skj-ar \
                        $(LIBC_SRC_FILES) $(LIBC_HDRS) build/rv/co_switch.o | build
	@mkdir -p build/lib/riscv/obj
	@for s in $(LIBC_SRCS); do \
	    ./build/skj-cc-rv $(LIBC_CFLAGS_RV) -o build/lib/riscv/obj/$$s.s libc/$$s.c && \
	    ./build/skj-as-rv -o build/lib/riscv/obj/$$s.o build/lib/riscv/obj/$$s.s \
	    || exit 1; \
	done
	@rm -f $@
	@./build/skj-ar rcs $@ $(addprefix build/lib/riscv/obj/,$(addsuffix .o,$(LIBC_SRCS))) build/rv/co_switch.o

build/lib/mips/libc.a: build/skj-cc-mips build/skj-as-mips build/skj-ar \
                       $(LIBC_SRC_FILES) $(LIBC_HDRS) build/mips/co_switch.o | build
	@mkdir -p build/lib/mips/obj
	@for s in $(LIBC_SRCS); do \
	    ./build/skj-cc-mips $(LIBC_CFLAGS_MIPS) -o build/lib/mips/obj/$$s.s libc/$$s.c && \
	    ./build/skj-as-mips -o build/lib/mips/obj/$$s.o build/lib/mips/obj/$$s.s \
	    || exit 1; \
	done
	@rm -f $@
	@./build/skj-ar rcs $@ $(addprefix build/lib/mips/obj/,$(addsuffix .o,$(LIBC_SRCS))) build/mips/co_switch.o

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

DRIVER_LIB_CF   := build/lib/coldfire/crt0.o build/lib/coldfire/half.o build/lib/coldfire/va_cf.o build/lib/coldfire/fini.o build/lib/coldfire/libc.a
DRIVER_LIB_RV   := build/lib/riscv/crt0.o build/lib/riscv/va_rv.o build/lib/riscv/soft64_div.o build/lib/riscv/fini.o build/lib/riscv/libc.a
DRIVER_LIB_MIPS := build/lib/mips/crt0.o build/lib/mips/half.o build/lib/mips/va_mips.o build/lib/mips/soft64_div.o build/lib/mips/fini.o build/lib/mips/libc.a

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
	 sh tests/run-libc-tests.sh

check-libc-rv: build/skj-cc-rv build/rv/start.o build/rv/va_rv.o build/rv/soft64_div.o build/rv/co_switch.o
	@CCBIN="$(CURDIR)/build/skj-cc-rv" \
	 ASM="$(RV_AS) -march=rv32imfd_zfh -mabi=ilp32 -o" \
	 LD="$(RV_LD) -m elf32lriscv -o" \
	 QEMU="$(QEMU_RV)" \
	 START="$(CURDIR)/build/rv/start.o" \
	 HALF="$(CURDIR)/build/rv/va_rv.o $(CURDIR)/build/rv/soft64_div.o $(CURDIR)/build/rv/co_switch.o" \
	 OUTDIR="$(CURDIR)/build/libc-rv" \
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
	 ASM="$(X64_ASM) -f elf64 -o" \
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
	 QEMU="$(QEMU_A64)" \
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

## Excelsior end to end on MIPS, o32 psABI, under qemu-mipsel.  The runtime
## is the same portable C the other tiers use, cross-compiled for mips1 o32:
## libexc, its native binding, utf8, and soft64 (the 64-bit helpers libexc's
## decimal path needs), plus start_mips_psabi.S for entry, syscalls, arena,
## and the source coroutines under the o32 convention.
EXC_MIPS_RT := build/mips/start_psabi.o build/mips/libexc.o \
               build/mips/exc_native.o build/mips/utf8.o build/mips/soft64.o

## libexc's double->int conversions lower to libgcc helpers (__fixdfsi, ...)
## since MIPS I has no trunc.w; the toolchain's libgcc supplies them.
MIPS_LIBGCC := $(shell $(MIPS_CC) -march=mips1 -EL -print-libgcc-file-name)

check-exc-mips: build/skj-exc-mips build/skj-run $(EXC_MIPS_RT)
	@BDIR="$(CURDIR)/build/mips/exc" \
	 EXC="$(CURDIR)/build/skj-exc-mips" \
	 AS="$(MIPS_AS) -march=mips1 -EL -o" \
	 LD="$(MIPS_LD) -o" \
	 START="$(CURDIR)/build/mips/start_psabi.o" \
	 LIBEXC="$(CURDIR)/build/mips/libexc.o" \
	 BINDING="$(CURDIR)/build/mips/exc_native.o" \
	 UTF8="$(CURDIR)/build/mips/utf8.o build/mips/soft64.o $(MIPS_LIBGCC)" \
	 sh tests/run-exc-tests.sh "$(QEMU_MIPS)"

## Excelsior under the noreorder scheduler (object model, sends, float, and
## the source coroutines through scheduled code).
check-exc-mips-noreorder: build/skj-exc-mips build/skj-run $(EXC_MIPS_RT)
	@SKJ_MIPS_NOREORDER=1 BDIR="$(CURDIR)/build/mips/exc-nr" \
	 EXC="$(CURDIR)/build/skj-exc-mips" \
	 AS="$(MIPS_AS) -march=mips1 -EL -o" \
	 LD="$(MIPS_LD) -o" \
	 START="$(CURDIR)/build/mips/start_psabi.o" \
	 LIBEXC="$(CURDIR)/build/mips/libexc.o" \
	 BINDING="$(CURDIR)/build/mips/exc_native.o" \
	 UTF8="$(CURDIR)/build/mips/utf8.o build/mips/soft64.o $(MIPS_LIBGCC)" \
	 sh tests/run-exc-tests.sh "$(QEMU_MIPS)"

## The whole MIPS tier in one target, the counterpart to check-emu-all, so CI
## and a MIPS working session have a single entry point.  Everything under
## qemu-mipsel with the GNU mipsel binutils: the front-end suites and the three
## IR-builder integration tests, the C and Excelsior suites (reorder and
## noreorder), the binutils-free toolchain (skj-as-mips / skj-ld-mips / skj-ar,
## the encoding golden-master, the archive and PS-EXE paths), and the FPU-less
## soft-float configuration.  test-mips-psexe-redux is excluded (it needs a
## real PlayStation BIOS and is opt-in).
check-mips-all: check-mips check-mips-noreorder \
                test-i64-mips test-ops-mips test-fpu-mips test-f32-mips \
                check-cc-mips check-cc-mips-noreorder \
                check-exc-mips check-exc-mips-noreorder \
                check-mipsas check-cc-mips-skj test-mips-archive test-mips-psexe \
                check-mips-sf
	@echo "All MIPS test suites passed."

check-cpp: build/skj-cpp
	@sh tests/run-cpp-tests.sh

## Excelsior end-to-end (skj-exc -> skj-as -> skj-ld -> qemu-m68k)
check-exc: build/skj-exc build/skj-as build/skj-ld build/start.o build/libexc.o build/exc_native.o build/utf8.o
	@sh tests/run-exc-tests.sh "$(QEMU)"

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

## Full qemu matrix across every supported architecture: the per-language
## suites, plus the IR-builder integration tests (fpu / i64 / unsigned ops).
## ColdFire (qemu-m68k), RISC-V (qemu-riscv32, hardware float), x86-64
## (qemu-x86_64), AArch64 (qemu-aarch64).
check-all: check check-cc check-cc-x86-64 check-cc-arm64 check-cc-rv check-cpp check-exc \
           check-exc-x86-64-asm check-exc-arm64-asm \
           check-rv check-x86-64 check-arm64 \
           check-x86-oracle check-arm64-oracle check-rv-oracle check-jit check-jit-arm64 \
           check-jit-rv check-jit-arm64-disasm check-jit-rv-disasm \
           test-jit-trap test-jit-trap-arm64 \
           check-exc-jit-arm64 test-libexc-guest32 test-jit-arena \
           test-gc test-coro test-threads test-exc-walker \
           test-fpu test-fpu-rv test-fpu-x86-64 test-fpu-arm64 \
           test-f32-diff \
           test-i64 test-i64-rv test-i64-x86-64 test-i64-arm64 \
           test-ops test-ops-rv test-ops-x86-64 test-ops-arm64 \
           check-rvas check-cc-rv-skj check-exc-rv-skj \
           check-cc-rv-psabi test-cc-rv-psabi-interop \
           test-cc-rv-psabi-gcc-stock test-cc-rv-psabi-archive \
           test-cc-rv-psabi-struct test-cc-driver check-libc \
           check-kp check-kpcom \
           check-kp-frontend check-kp-jit check-kp-aot check-kp-corpus check-kp-multi check-kp-io check-kp-inc
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

.PHONY: all release-check check check-rv run-rv test-rv-irq test-rv-run test-guest-mmap test-rv-sandbox test-rv-xlang-sandbox test-rv-decode-cache test-rv-expand test-rv-bus test-rv-csr test-rv-decode test-rv-fp check-archtest check-rv32 test-rv32-apps test-rv32-unit test-rv32-program test-rv32-lockstep test-rv32-fuzz audit-rv32-coverage audit-rv32-icov audit-rv32-mutants check-exc-rv check-exc-rv-asm check-exc-rv-emu check-exc-rv-emu-san test-exc-coro-overflow check-emu check-rv-emu check-exc-emu check-emu-all check-emu-san bench bench-cf bench-rv bench-accept test-fpu-emu test-i64-emu test-ops-emu test-exc-walker-emu check-x86-64 check-arm64 check-x86-oracle check-arm64-oracle check-rv-oracle check-jit check-jit-arm64 check-jit-rv check-jit-arm64-disasm check-jit-rv-disasm test-jit-trap test-jit-trap-arm64 check-all check-cc check-cc-x86-64 check-cc-arm64 check-cc-rv check-cpp check-exc check-exc-x86-64-asm check-exc-arm64-asm check-as check-rvas check-cc-rv-skj check-exc-rv-skj check-cc-rv-psabi test-cc-rv-psabi-interop test-cc-rv-psabi-gcc-stock test-cc-rv-psabi-archive test-cc-rv-psabi-struct test-cc-driver check-libc check-libc-cf check-libc-mips check-libc-rv check-libc-x86-64 check-libc-arm64 check-mipsas check-cc-mips-skj check-mips-all check-smoke check-exc-jit-arm64 test-gc test-coro test-threads test-libexc-guest32 test-jit-arena test-fpu test-fpu-rv test-fpu-x86-64 test-fpu-arm64 test-f32-rv f32-oracle f32-golden-check test-f32-diff test-f32-diff-rv test-f32-diff-x86-64 test-f32-diff-arm64 test-f32-diff-mips test-f32-diff-cf test-torture-diff test-i64 test-i64-rv test-i64-x86-64 test-i64-arm64 test-ops test-ops-rv test-ops-x86-64 test-ops-arm64 test-parse update-kpcom check-kpcom bootstrap-fpc check-kp-frontend check-kp-jit check-kp-aot check-kp-jit-rv check-kp-jit-arm64 check-kp-aot-rv check-kp-aot-arm64 check-kp-free check-kp-corpus check-kp-corpus-rv check-kp-corpus-arm64 check-kp-multi check-kp-multi-rv check-kp-multi-arm64 check-kp-io check-kp-io-rv check-kp-io-arm64 check-kp-io-free check-kp-inc check-kp-inc-rv check-kp-inc-arm64 check-kp-inc-free coverage-codegen clean FORCE
