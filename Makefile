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

CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -Wpedantic -Wno-unused-parameter
CFLAGS  += -Ibuild

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
MIPS_AS ?= mipsel-none-elf-as
MIPS_LD ?= mipsel-none-elf-ld
MIPS_OBJCOPY ?= mipsel-none-elf-objcopy
MIPS_OBJDUMP ?= mipsel-none-elf-objdump
QEMU_MIPS ?= qemu-mipsel
SPIM ?= spim
X86_ASM ?= nasm
X86_LD  ?= ld
QEMU_X86 ?= qemu-i386
A64_AS  ?= aarch64-linux-gnu-as
A64_LD  ?= aarch64-linux-gnu-ld
A64_CC  ?= aarch64-linux-gnu-gcc
QEMU_A64 ?= qemu-aarch64
X64_ASM ?= nasm
X64_LD  ?= ld
QEMU_X64 ?= qemu-x86_64

IR_SRC  := ir/ir.c ir/util.c ir/arena.c
BE_CF   := backend/regalloc_cf.c backend/cf_emit.c
BE_RV   := backend/regalloc_rv.c backend/rv_emit.c
BE_MIPS := backend/regalloc_mips.c backend/mips_emit.c
BE_X86  := backend/regalloc_x86.c backend/x86_emit.c
BE_A64  := backend/regalloc_arm64.c backend/arm64_emit.c
BE_X64  := backend/regalloc_x86.c backend/x86_emit.c
TC_SRC  := tinc/lex.c tinc/parse.c tinc/lower.c tinc/main.c
SRC     := $(IR_SRC) $(BE_CF) $(TC_SRC)
SRC_RV  := $(IR_SRC) $(BE_RV) $(TC_SRC)
SRC_MIPS := $(IR_SRC) $(BE_MIPS) $(TC_SRC)
SRC_X86 := $(IR_SRC) $(BE_X86) $(TC_SRC)
SRC_A64 := $(IR_SRC) $(BE_A64) $(TC_SRC)
SRC_X64 := $(IR_SRC) $(BE_X64) $(TC_SRC)

TESTS     := $(wildcard tests/*.tc)
SCM_TESTS := $(wildcard tests/scm_*.scm)
MOO_TESTS := $(filter-out tests/moo_room.moo tests/moo_toy_%.moo,$(wildcard tests/moo_*.moo))
MOO_TOY_TESTS := $(wildcard tests/moo_toy_*.moo)
PAS_TESTS := $(wildcard tests/pascal_*.pas)

TOOLS := build/skj-tinc build/skj-tinc-rv build/skj-tinc-mips build/skj-tinc-x86 build/skj-tinc-arm64 \
         build/skj-sc build/skj-sc-rv build/skj-sc-mips build/skj-sc-x86 build/skj-sc-arm64 \
         build/skj-mooc build/skj-pc build/skj-as build/skj-ld \
         build/skj-as-rv build/skj-ld-rv build/skj-as-mips build/skj-ld-mips build/skj-ar \
         build/skj-cpp build/skj-cc build/skj-cc-x86-64 build/skj-cc-arm64 \
         build/skj-cc-rv build/skj-cc-rv-psabi build/skj-run

all: $(TOOLS)

build/skj-tinc: $(SRC) ir/ir.h tinc/tinc.h | build
	$(CC) $(CFLAGS) -Iir -Itinc -o $@ $(SRC)

build/skj-tinc-rv: $(SRC_RV) ir/ir.h tinc/tinc.h | build
	$(CC) $(CFLAGS) -Iir -Itinc -o $@ $(SRC_RV)

build/skj-tinc-mips: $(SRC_MIPS) ir/ir.h tinc/tinc.h | build
	$(CC) $(CFLAGS) -Iir -Itinc -o $@ $(SRC_MIPS)

build/skj-tinc-x86: $(SRC_X86) ir/ir.h tinc/tinc.h | build
	$(CC) $(CFLAGS) -Iir -Itinc -o $@ $(SRC_X86)

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
CC_SRC_X64 := $(CC_FE) $(CC_CPP) $(IR_SRC) $(BE_X64)
CC_SRC_A64 := $(CC_FE) $(CC_CPP) $(IR_SRC) $(BE_A64)
CC_SRC_RV := $(CC_FE) $(CC_CPP) $(IR_SRC) $(BE_RV)
CC_SRC_MIPS := $(CC_FE) $(CC_CPP) $(IR_SRC) $(BE_MIPS)

build/skj-cc: $(CC_SRC) cc/cc.h cpp/cpp.h cpp/internal.h ir/ir.h | build
	$(CC) $(CFLAGS) -DCC_M68K -Icc -Icpp -Iir -o $@ $(CC_SRC)

build/skj-cc-x86-64: $(CC_SRC_X64) cc/cc.h cpp/cpp.h cpp/internal.h ir/ir.h | build
	$(CC) $(CFLAGS) -DX86_BITS=64 -DCC_LP64 -DCC_PSABI -DCC_STRUCT_ABI -Icc -Icpp -Iir -o $@ $(CC_SRC_X64)

build/skj-cc-arm64: $(CC_SRC_A64) cc/cc.h cpp/cpp.h cpp/internal.h ir/ir.h | build
	$(CC) $(CFLAGS) -DCC_LP64 -DCC_PSABI -DCC_ARM64 -Icc -Icpp -Iir -o $@ $(CC_SRC_A64)

build/skj-cc-rv: $(CC_SRC_RV) cc/cc.h cpp/cpp.h cpp/internal.h ir/ir.h | build
	$(CC) $(CFLAGS) -Icc -Icpp -Iir -o $@ $(CC_SRC_RV)

## The psABI build of the RISC-V C compiler: -DCC_PSABI emits the standard
## RISC-V ILP32 register convention (a0..a7) instead of the toolkit's stack
## convention, so its output interlinks with gcc-built objects.  The 64-bit
## integer helpers stay an internal stack-passed backend/runtime ABI either
## way (see runtime/start_rv_psabi_cc.S).
build/skj-cc-rv-psabi: $(CC_SRC_RV) cc/cc.h cpp/cpp.h cpp/internal.h ir/ir.h | build
	$(CC) $(CFLAGS) -DCC_PSABI -Icc -Icpp -Iir -o $@ $(CC_SRC_RV)

build/skj-cc-mips: $(CC_SRC_MIPS) cc/cc.h cpp/cpp.h cpp/internal.h ir/ir.h | build
	$(CC) $(CFLAGS) -Icc -Icpp -Iir -o $@ $(CC_SRC_MIPS)

## The PlayStation (R3051, no coprocessor 1) soft-float C compiler: the same
## sources with -DMIPS_SOFTFLOAT, which emits no cop-1 instruction and lowers
## every float op to a runtime/softfloat.c call.
build/skj-cc-mips-sf: $(CC_SRC_MIPS) cc/cc.h cpp/cpp.h cpp/internal.h ir/ir.h | build
	$(CC) $(CFLAGS) -DMIPS_SOFTFLOAT -Icc -Icpp -Iir -o $@ $(CC_SRC_MIPS)

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
## Built up in steps; step 2 encodes the instruction set.  See doc/mips.md.
AS_MIPS_SRC := as/mips_main.c as/asm_lex.c as/asm_obj.c as/mips_parse.c \
               as/mips_encode.c as/mips_elf.c as/mips_sched.c ir/util.c ir/arena.c

build/skj-as-mips: $(AS_MIPS_SRC) as/mips.h as/asm_lex.h as/asm_obj.h | build
	$(CC) $(CFLAGS) -Ias -Iir -o $@ $(AS_MIPS_SRC)

## skj-ld linker
LD_SRC := ld/main.c ld/elf_read.c ld/script.c ld/link.c ld/elf_write.c ld/mapfile.c ir/util.c ir/arena.c

build/skj-ld: $(LD_SRC) ld/ld.h ld/mapfile.h | build
	$(CC) $(CFLAGS) -Ild -Iir -o $@ $(LD_SRC)

## RISC-V RV32 linker (shares layout core, script parser and mapfile reader)
LD_RV_SRC := ld/rv_main.c ld/rv_elf_read.c ld/rv_link.c ld/rv_elf_write.c \
             ld/rv_archive.c ld/link.c ld/script.c ld/mapfile.c \
             ir/util.c ir/arena.c

build/skj-ld-rv: $(LD_RV_SRC) ld/ld.h ld/rv_ld.h ld/mapfile.h | build
	$(CC) $(CFLAGS) -Ild -Iir -o $@ $(LD_RV_SRC)

## MIPS I linker (shares layout core, script parser and mapfile reader).
## REL relocations with the o32 HI16/LO16 addend carry-pairing.  See doc/mips.md.
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

## Every suite the in-tree emulator can run, no qemu involved.
check-emu-all: check-emu check-rv-emu check-exc-emu test-exc-walker-emu test-rv-irq \
               test-rv-expand test-rv-bus test-rv-csr test-rv-decode test-rv-fp \
               check-rv32 \
               test-fpu-emu test-i64-emu test-ops-emu
	@echo "All emulator test suites passed."

## x86 per-test rules: tests/<name>.tc -> build/x86/<name>.s -> .o -> binary
X86_TESTS := $(TESTS)
X86_SCM_TESTS := $(SCM_TESTS)

build/x86/%.s: tests/%.tc build/skj-tinc-x86 | build/x86
	./build/skj-tinc-x86 -o $@ $<

build/x86/scm_%.s: tests/scm_%.scm build/skj-sc-x86 | build/x86
	./build/skj-sc-x86 -o $@ $<

build/x86/%.o: build/x86/%.s
	$(X86_ASM) -f elf32 -o $@ $<

build/x86/start.o: runtime/start_x86.asm | build/x86
	$(X86_ASM) -f elf32 -o $@ $<

build/x86/%: build/x86/%.o build/x86/start.o
	$(X86_LD) -m elf_i386 -o $@ build/x86/start.o $<

build/x86:
	mkdir -p build/x86

check-x86: build/skj-tinc-x86 build/skj-sc-x86 $(X86_TESTS:tests/%.tc=build/x86/%) $(X86_SCM_TESTS:tests/%.scm=build/x86/%)
	@sh tests/run-tests.sh "$(QEMU_X86)" build/x86

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
SC_SRC_X86 := scheme/lex.c scheme/parse.c scheme/print.c scheme/main.c scheme/gc.c \
              scheme/lower.c $(IR_SRC) $(BE_X86)
SC_SRC_A64 := scheme/lex.c scheme/parse.c scheme/print.c scheme/main.c scheme/gc.c \
              scheme/lower.c $(IR_SRC) $(BE_A64)

build/skj-sc: $(SC_SRC) scheme/scheme.h scheme/gc.h ir/ir.h | build
	$(CC) $(CFLAGS) -Ischeme -Iir -o $@ $(SC_SRC)

build/skj-sc-rv: $(SC_SRC_RV) scheme/scheme.h scheme/gc.h ir/ir.h | build
	$(CC) $(CFLAGS) -Ischeme -Iir -o $@ $(SC_SRC_RV)

build/skj-sc-mips: $(SC_SRC_MIPS) scheme/scheme.h scheme/gc.h ir/ir.h | build
	$(CC) $(CFLAGS) -Ischeme -Iir -o $@ $(SC_SRC_MIPS)

build/skj-sc-x86: $(SC_SRC_X86) scheme/scheme.h scheme/gc.h ir/ir.h | build
	$(CC) $(CFLAGS) -Ischeme -Iir -o $@ $(SC_SRC_X86)

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

## RV32 interrupt delivery (host-only: it drives the core directly, so
## it needs neither a cross toolchain nor qemu)
build/test_rv_irq: tests/test_rv_irq.c emu/rv32.c emu/rv32.h | build
	$(CC) $(CFLAGS) -Iemu -o $@ tests/test_rv_irq.c emu/rv32.c -lm

test-rv-irq: build/test_rv_irq
	@./build/test_rv_irq

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
RV32_UNIT := test_mem test_fp test_atomic test_zcmp test_zcb \
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
## Built with the smolmoo SDK convention (ColdFire V4e, gcc-driven link
## against runtime/smolmoo.ld); runtime/soft64.c supplies the 64-bit
## helpers start.S provides on the qemu tier, since the toolchain's
## libgcc is 68020 code (no ColdFire multilib) and is deliberately not
## linked: a missing helper is a link error, never silent 68020 code.
## Link-only smoke in this tree: the ELF loads under the smolmoo
## server, not qemu (its LINE_A hypercalls have no Linux meaning).
## `make smolmoo-demo` proves the pipeline.
SMOLMOO_CFLAGS := -mcpu=5475 -O2 -Wall -ffreestanding

build/smolmoo:
	mkdir -p build/smolmoo

build/smolmoo/libexc.o: runtime/libexc.c runtime/libexc.h runtime/utf8.h | build/smolmoo
	$(M68K_CC) -std=c99 $(SMOLMOO_CFLAGS) -c -o $@ $<

build/smolmoo/utf8.o: runtime/utf8.c runtime/utf8.h | build/smolmoo
	$(M68K_CC) -std=c99 $(SMOLMOO_CFLAGS) -c -o $@ $<

build/smolmoo/exc_smolmoo.o: runtime/exc_smolmoo.c runtime/libexc.h | build/smolmoo
	$(M68K_CC) -std=c99 $(SMOLMOO_CFLAGS) -c -o $@ $<

build/smolmoo/smolmoo_rt.o: runtime/smolmoo_rt.S | build/smolmoo
	$(M68K_CC) $(SMOLMOO_CFLAGS) -c -o $@ $<

build/smolmoo/demo.s: build/skj-exc tests/exs_source_yield.exs | build/smolmoo
	./build/skj-exc -o $@ tests/exs_source_yield.exs

build/smolmoo/demo.o: build/smolmoo/demo.s
	$(M68K_CC) $(SMOLMOO_CFLAGS) -c -o $@ $<

build/smolmoo/soft64.o: runtime/soft64.c | build/smolmoo
	$(M68K_CC) -std=c99 $(SMOLMOO_CFLAGS) -c -o $@ $<

build/smolmoo/demo.elf: build/smolmoo/demo.o build/smolmoo/libexc.o \
		build/smolmoo/exc_smolmoo.o build/smolmoo/smolmoo_rt.o \
		build/smolmoo/soft64.o build/smolmoo/utf8.o runtime/smolmoo.ld
	$(M68K_CC) -mcpu=5475 -nostdlib -static -T runtime/smolmoo.ld -o $@ \
		build/smolmoo/demo.o build/smolmoo/libexc.o \
		build/smolmoo/exc_smolmoo.o build/smolmoo/smolmoo_rt.o \
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

## I64 integration test (IR builder → x86 asm → qemu-i386)
I64_TEST_X86_SRC := tests/test_i64.c $(IR_SRC) $(BE_X86)

build/test_i64_x86: $(I64_TEST_X86_SRC) ir/ir.h | build
	$(CC) $(CFLAGS) -Iir -o $@ $(I64_TEST_X86_SRC)

build/x86/i64_test.s: build/test_i64_x86 | build/x86
	./build/test_i64_x86 -o $@

build/x86/i64_test.o: build/x86/i64_test.s
	$(X86_ASM) -f elf32 -o $@ $<

build/x86/i64_test: build/x86/i64_test.o build/x86/start.o
	$(X86_LD) -m elf_i386 -o $@ build/x86/start.o $<

test-i64-x86: build/x86/i64_test
	@echo "Running I64 test under qemu-i386..."
	@$(QEMU_X86) ./build/x86/i64_test && echo "PASS: i64_test (x86)" || (echo "FAIL: i64_test (x86)"; exit 1)

## Unsigned 32-bit ops integration test (IR builder → x86 asm → qemu-i386)
OPS_TEST_X86_SRC := tests/test_ops.c $(IR_SRC) $(BE_X86)

build/test_ops_x86: $(OPS_TEST_X86_SRC) ir/ir.h | build
	$(CC) $(CFLAGS) -Iir -o $@ $(OPS_TEST_X86_SRC)

build/x86/ops_test.s: build/test_ops_x86 | build/x86
	./build/test_ops_x86 -o $@

build/x86/ops_test.o: build/x86/ops_test.s
	$(X86_ASM) -f elf32 -o $@ $<

build/x86/ops_test: build/x86/ops_test.o build/x86/start.o
	$(X86_LD) -m elf_i386 -o $@ build/x86/start.o $<

test-ops-x86: build/x86/ops_test
	@echo "Running unsigned ops test under qemu-i386..."
	@$(QEMU_X86) ./build/x86/ops_test && echo "PASS: ops_test (x86)" || (echo "FAIL: ops_test (x86)"; exit 1)

## FPU integration test (IR builder → x86 asm → qemu-i386)
FPU_TEST_X86_SRC := tests/test_fpu.c $(IR_SRC) $(BE_X86)

build/test_fpu_x86: $(FPU_TEST_X86_SRC) ir/ir.h | build
	$(CC) $(CFLAGS) -Iir -o $@ $(FPU_TEST_X86_SRC)

build/x86/fpu_test.s: build/test_fpu_x86 | build/x86
	./build/test_fpu_x86 -o $@

build/x86/fpu_test.o: build/x86/fpu_test.s
	$(X86_ASM) -f elf32 -o $@ $<

build/x86/half.o: runtime/half.c | build/x86
	$(CC) -m32 -std=c99 -O2 -Wall -ffreestanding -c -o $@ $<

build/x86/fpu_test: build/x86/fpu_test.o build/x86/start.o build/x86/half.o
	$(X86_LD) -m elf_i386 -o $@ build/x86/start.o build/x86/half.o $<

test-fpu-x86: build/x86/fpu_test
	@echo "Running FPU test under qemu-i386..."
	@$(QEMU_X86) ./build/x86/fpu_test && echo "PASS: fpu_test (x86)" || (echo "FAIL: fpu_test (x86)"; exit 1)

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

build/x86_64/fpu_test: build/x86_64/fpu_test.o build/x86_64/start.o build/x86_64/half.o
	$(X64_LD) -m elf_x86_64 -o $@ build/x86_64/start.o build/x86_64/half.o $<

test-fpu-x86-64: build/x86_64/fpu_test
	@echo "Running FPU test under qemu-x86_64..."
	@$(QEMU_X64) ./build/x86_64/fpu_test && echo "PASS: fpu_test (x86-64)" || (echo "FAIL: fpu_test (x86-64)"; exit 1)

## Native single-precision (IR_F32) integration test on x86-32 and x86-64
F32_TEST_X86_SRC := tests/test_f32.c $(IR_SRC) $(BE_X86)
build/test_f32_x86: $(F32_TEST_X86_SRC) ir/ir.h | build
	$(CC) $(CFLAGS) -Iir -o $@ $(F32_TEST_X86_SRC)
build/x86/f32_test.s: build/test_f32_x86 | build/x86
	./build/test_f32_x86 -o $@
build/x86/f32_test.o: build/x86/f32_test.s
	$(X86_ASM) -f elf32 -o $@ $<
build/x86/f32_test: build/x86/f32_test.o build/x86/start.o
	$(X86_LD) -m elf_i386 -o $@ build/x86/start.o $<
test-f32-x86: build/x86/f32_test
	@echo "Running F32 test under qemu-i386..."
	@$(QEMU_X86) ./build/x86/f32_test && echo "PASS: f32_test (x86)" || (echo "FAIL: f32_test (x86)"; exit 1)

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
# psABI feature tests, excluded where the target lacks that phase.  Varargs is
# done on x86-64 (P2) and arm64 (P4-P2); by-value struct passing is x86-64 only
# (P3).  ColdFire and RISC-V use the stack convention and have neither.
# The psABI cc tests (varargs, struct-by-value) run on x86-64 and arm64 and are
# excluded only on the stack-convention targets (ColdFire, RISC-V).
VARARGS_EXCLUDE   := cc_t071_varargs
STRUCT_EXCLUDE    := cc_t072_struct_byval
REGSTRUCT_EXCLUDE := cc_t074_hfa cc_t075_struct_straddle
NOPSABI_EXCLUDE   := $(VARARGS_EXCLUDE) $(STRUCT_EXCLUDE) $(REGSTRUCT_EXCLUDE)

check-cc: build/skj-cc build/skj-as build/start.o build/half.o
	@OUTDIR="$(CURDIR)/build/cc-cf" EXCLUDE="$(NOPSABI_EXCLUDE)" \
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
check-cc-rv: build/skj-cc-rv build/rv/start.o
	@CCBIN="$(CURDIR)/build/skj-cc-rv" \
	 ASM="$(RV_AS) -march=rv32imfd_zfh -mabi=ilp32 -o" \
	 LD="$(RV_LD) -m elf32lriscv -o" \
	 QEMU="$(QEMU_RV)" \
	 START="$(CURDIR)/build/rv/start.o" \
	 HALF="" \
	 OUTDIR="$(CURDIR)/build/cc-rv" \
	 EXCLUDE="$(NOPSABI_EXCLUDE)" \
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

check-cc-rv-skj: build/skj-cc-rv build/skj-as-rv build/skj-ld-rv build/rv-skj/start.o
	@CCBIN="$(CURDIR)/build/skj-cc-rv" \
	 ASM="$(CURDIR)/build/skj-as-rv -o" \
	 LD="$(CURDIR)/build/skj-ld-rv -o" \
	 QEMU="$(QEMU_RV)" \
	 START="$(CURDIR)/build/rv-skj/start.o" \
	 HALF="" \
	 OUTDIR="$(CURDIR)/build/cc-rv-skj" \
	 EXCLUDE="$(NOPSABI_EXCLUDE)" \
	 sh tests/run-cc-tests.sh

## The psABI C suite end to end through the in-tree RV32 toolchain:
## skj-cc-rv-psabi emits the standard RISC-V ILP32 convention, assembled by
## skj-as-rv and linked by skj-ld-rv over the psABI crt, run under qemu-riscv32.
## The crt (start_rv_psabi_cc.o) is assembled by skj-as-rv.  varargs and
## by-value structs are still excluded: the RV backend implements the scalar
## psABI (a0..a7, i64 register pairs, results in a0/a1), not the struct-passing
## rules, so those tests wait on RV struct-ABI support.
build/rv-skj/start_psabi_cc.o: runtime/start_rv_psabi_cc.S build/skj-as-rv | build/rv-skj
	./build/skj-as-rv -o $@ $<

check-cc-rv-psabi: build/skj-cc-rv-psabi build/skj-as-rv build/skj-ld-rv build/rv-skj/start_psabi_cc.o
	@CCBIN="$(CURDIR)/build/skj-cc-rv-psabi" \
	 ASM="$(CURDIR)/build/skj-as-rv -o" \
	 LD="$(CURDIR)/build/skj-ld-rv -o" \
	 QEMU="$(QEMU_RV)" \
	 START="$(CURDIR)/build/rv-skj/start_psabi_cc.o" \
	 HALF="" \
	 OUTDIR="$(CURDIR)/build/cc-rv-psabi" \
	 EXCLUDE="$(NOPSABI_EXCLUDE)" \
	 sh tests/run-cc-tests.sh

## psABI interop proof: a skj-cc-rv-psabi main calls a gcc-built RV32 function
## (i64 and int arguments, i64 result) and the whole program is linked by
## skj-ld-rv.  This is the point of the psABI build, that its output follows
## the standard ABI a stock RISC-V toolchain emits.
test-cc-rv-psabi-interop: build/skj-cc-rv-psabi build/skj-as-rv build/skj-ld-rv build/rv-skj/start_psabi_cc.o | build/rv-skj
	@$(RV_CC) $(RV_SKJ_RT_CFLAGS) -c -o build/rv-skj/interop_gcc.o tests/rv_psabi_interop_gcc.c
	@./build/skj-cc-rv-psabi -o build/rv-skj/interop_main.s tests/rv_psabi_interop_main.c
	@./build/skj-as-rv -o build/rv-skj/interop_main.o build/rv-skj/interop_main.s
	@./build/skj-ld-rv -o build/rv-skj/interop build/rv-skj/start_psabi_cc.o \
	    build/rv-skj/interop_main.o build/rv-skj/interop_gcc.o
	@$(QEMU_RV) ./build/rv-skj/interop; ec=$$?; \
	 if [ $$ec -eq 42 ]; then echo "PASS: cc-rv-psabi interop (skj main + gcc fn, exit 42)"; \
	 else echo "FAIL: cc-rv-psabi interop (exit $$ec, expected 42)"; exit 1; fi

## Stock-gcc interlink: a gcc object built with the default flags (PIE and
## linker relaxation, so R_RISCV_GOT_HI20 and R_RISCV_CALL_PLT) linked into a
## static executable by skj-ld-rv, which synthesizes the GOT.  Proves the
## toolchain interlinks stock gcc objects, not only ones built -fno-pic
## -mno-relax.
test-cc-rv-psabi-gcc-stock: build/skj-cc-rv-psabi build/skj-as-rv build/skj-ld-rv build/rv-skj/start_psabi_cc.o | build/rv-skj
	@$(RV_CC) $(RV_RT_CFLAGS) -c -o build/rv-skj/stock_lib.o tests/rv_gcc_stock_lib.c
	@./build/skj-cc-rv-psabi -o build/rv-skj/stock_main.s tests/rv_gcc_stock_main.c
	@./build/skj-as-rv -o build/rv-skj/stock_main.o build/rv-skj/stock_main.s
	@./build/skj-ld-rv -o build/rv-skj/stock build/rv-skj/start_psabi_cc.o \
	    build/rv-skj/stock_main.o build/rv-skj/stock_lib.o
	@$(QEMU_RV) ./build/rv-skj/stock; ec=$$?; \
	 if [ $$ec -eq 42 ]; then echo "PASS: cc-rv-psabi stock-gcc interlink (GOT + CALL_PLT, exit 42)"; \
	 else echo "FAIL: cc-rv-psabi stock-gcc interlink (exit $$ec, expected 42)"; exit 1; fi

## Archive (.a) support: skj-ld-rv pulls the members that resolve undefined
## symbols (member A, and B transitively) and skips the unreferenced one (whose
## own undefined symbol would break the link if it were wrongly pulled).
test-cc-rv-psabi-archive: build/skj-cc-rv-psabi build/skj-as-rv build/skj-ld-rv build/rv-skj/start_psabi_cc.o | build/rv-skj
	@$(RV_CC) $(RV_RT_CFLAGS) -c -o build/rv-skj/arch_a.o tests/rv_archive_a.c
	@$(RV_CC) $(RV_RT_CFLAGS) -c -o build/rv-skj/arch_b.o tests/rv_archive_b.c
	@$(RV_CC) $(RV_RT_CFLAGS) -c -o build/rv-skj/arch_unused.o tests/rv_archive_unused.c
	@rm -f build/rv-skj/libarch.a
	@$(RV_AR) rcs build/rv-skj/libarch.a build/rv-skj/arch_a.o build/rv-skj/arch_b.o \
	    build/rv-skj/arch_unused.o
	@./build/skj-cc-rv-psabi -o build/rv-skj/arch_main.s tests/rv_archive_main.c
	@./build/skj-as-rv -o build/rv-skj/arch_main.o build/rv-skj/arch_main.s
	@./build/skj-ld-rv -o build/rv-skj/arch build/rv-skj/start_psabi_cc.o \
	    build/rv-skj/arch_main.o build/rv-skj/libarch.a
	@$(QEMU_RV) ./build/rv-skj/arch; ec=$$?; \
	 if [ $$ec -eq 42 ]; then echo "PASS: cc-rv-psabi archive link (member pulled from .a, unused skipped, exit 42)"; \
	 else echo "FAIL: cc-rv-psabi archive link (exit $$ec, expected 42)"; exit 1; fi

## C compiler tests on MIPS I (skj-cc-mips, qemu-mipsel).  Stack convention
## like RISC-V, so the psABI-only tests are excluded; half.o supplies
## _Float16 (MIPS I has no hardware half).
check-cc-mips: build/skj-cc-mips build/mips/start.o build/mips/half.o
	@CCBIN="$(CURDIR)/build/skj-cc-mips" \
	 ASM="$(MIPS_AS) -march=mips1 -EL -o" \
	 LD="$(MIPS_LD) -o" \
	 QEMU="$(QEMU_MIPS)" \
	 START="$(CURDIR)/build/mips/start.o" \
	 HALF="$(CURDIR)/build/mips/half.o" \
	 OUTDIR="$(CURDIR)/build/cc-mips" \
	 EXCLUDE="$(NOPSABI_EXCLUDE)" \
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
check-cc-mips-skj: build/skj-cc-mips build/skj-as-mips build/skj-ld-mips build/mips/start.o build/mips/half.o
	@CCBIN="$(CURDIR)/build/skj-cc-mips" \
	 ASM="$(CURDIR)/build/skj-as-mips -o" \
	 LD="$(CURDIR)/build/skj-ld-mips -o" \
	 QEMU="$(QEMU_MIPS)" \
	 START="$(CURDIR)/build/mips/start.o" \
	 HALF="$(CURDIR)/build/mips/half.o" \
	 OUTDIR="$(CURDIR)/build/cc-mips-skj" \
	 EXCLUDE="$(NOPSABI_EXCLUDE)" \
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
check-cc-mips-noreorder: build/skj-cc-mips build/mips/start.o build/mips/half.o
	@SKJ_MIPS_NOREORDER=1 CCBIN="$(CURDIR)/build/skj-cc-mips" \
	 ASM="$(MIPS_AS) -march=mips1 -EL -o" \
	 LD="$(MIPS_LD) -o" \
	 QEMU="$(QEMU_MIPS)" \
	 START="$(CURDIR)/build/mips/start.o" \
	 HALF="$(CURDIR)/build/mips/half.o" \
	 OUTDIR="$(CURDIR)/build/cc-mips-nr" \
	 EXCLUDE="$(NOPSABI_EXCLUDE)" \
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
	 EXCLUDE="$(NOPSABI_EXCLUDE)" \
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
## ColdFire (qemu-m68k), RISC-V (qemu-riscv32, hardware float), x86-32
## (qemu-i386), AArch64 (qemu-aarch64).
check-all: check check-cc check-cc-x86-64 check-cc-arm64 check-cc-rv check-cpp check-exc \
           check-rv check-x86 check-x86-64 check-arm64 \
           test-gc test-exc-walker \
           test-fpu test-fpu-rv test-fpu-x86 test-fpu-x86-64 test-fpu-arm64 \
           test-i64 test-i64-rv test-i64-x86 test-i64-x86-64 test-i64-arm64 \
           test-ops test-ops-rv test-ops-x86 test-ops-x86-64 test-ops-arm64 \
           check-rvas check-cc-rv-skj check-exc-rv-skj \
           check-cc-rv-psabi test-cc-rv-psabi-interop \
           test-cc-rv-psabi-gcc-stock test-cc-rv-psabi-archive
	@echo "All architecture test suites passed."

clean:
	rm -rf build

.PHONY: all release-check check check-rv test-rv-irq test-rv-expand test-rv-bus test-rv-csr test-rv-decode test-rv-fp check-archtest check-rv32 test-rv32-apps test-rv32-unit test-rv32-program test-rv32-lockstep test-rv32-fuzz audit-rv32-coverage audit-rv32-icov audit-rv32-mutants check-exc-rv check-exc-rv-asm check-emu check-rv-emu check-exc-emu check-emu-all test-fpu-emu test-i64-emu test-ops-emu test-exc-walker-emu check-x86 check-x86-64 check-arm64 check-all check-cc check-cc-x86-64 check-cc-arm64 check-cc-rv check-cpp check-exc check-as check-rvas check-cc-rv-skj check-exc-rv-skj check-cc-rv-psabi test-cc-rv-psabi-interop test-cc-rv-psabi-gcc-stock test-cc-rv-psabi-archive check-mipsas check-cc-mips-skj check-smoke test-gc test-fpu test-fpu-rv test-fpu-x86 test-fpu-x86-64 test-fpu-arm64 test-f32-rv test-i64 test-i64-rv test-i64-x86 test-i64-x86-64 test-i64-arm64 test-ops test-ops-rv test-ops-x86 test-ops-x86-64 test-ops-arm64 test-parse clean FORCE
