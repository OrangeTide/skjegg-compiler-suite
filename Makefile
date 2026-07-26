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
BE_X86  := backend/regalloc_x86.c backend/x86_emit.c
BE_A64  := backend/regalloc_arm64.c backend/arm64_emit.c
BE_X64  := backend/regalloc_x86.c backend/x86_emit.c
TC_SRC  := tinc/lex.c tinc/parse.c tinc/lower.c tinc/main.c
SRC     := $(IR_SRC) $(BE_CF) $(TC_SRC)
SRC_RV  := $(IR_SRC) $(BE_RV) $(TC_SRC)
SRC_X86 := $(IR_SRC) $(BE_X86) $(TC_SRC)
SRC_A64 := $(IR_SRC) $(BE_A64) $(TC_SRC)
SRC_X64 := $(IR_SRC) $(BE_X64) $(TC_SRC)

TESTS     := $(wildcard tests/*.tc)
SCM_TESTS := $(wildcard tests/scm_*.scm)
MOO_TESTS := $(filter-out tests/moo_room.moo tests/moo_toy_%.moo,$(wildcard tests/moo_*.moo))
MOO_TOY_TESTS := $(wildcard tests/moo_toy_*.moo)
PAS_TESTS := $(wildcard tests/pascal_*.pas)

TOOLS := build/skj-tinc build/skj-tinc-rv build/skj-tinc-x86 build/skj-tinc-arm64 \
         build/skj-sc build/skj-sc-rv build/skj-sc-x86 build/skj-sc-arm64 \
         build/skj-mooc build/skj-pc build/skj-as build/skj-ld \
         build/skj-cpp build/skj-cc build/skj-cc-x86-64 build/skj-cc-arm64 \
         build/skj-cc-rv

all: $(TOOLS)

build/skj-tinc: $(SRC) ir/ir.h tinc/tinc.h | build
	$(CC) $(CFLAGS) -Iir -Itinc -o $@ $(SRC)

build/skj-tinc-rv: $(SRC_RV) ir/ir.h tinc/tinc.h | build
	$(CC) $(CFLAGS) -Iir -Itinc -o $@ $(SRC_RV)

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

build/skj-cc: $(CC_SRC) cc/cc.h cpp/cpp.h cpp/internal.h ir/ir.h | build
	$(CC) $(CFLAGS) -DCC_M68K -Icc -Icpp -Iir -o $@ $(CC_SRC)

build/skj-cc-x86-64: $(CC_SRC_X64) cc/cc.h cpp/cpp.h cpp/internal.h ir/ir.h | build
	$(CC) $(CFLAGS) -DX86_BITS=64 -DCC_LP64 -DCC_PSABI -DCC_STRUCT_ABI -Icc -Icpp -Iir -o $@ $(CC_SRC_X64)

build/skj-cc-arm64: $(CC_SRC_A64) cc/cc.h cpp/cpp.h cpp/internal.h ir/ir.h | build
	$(CC) $(CFLAGS) -DCC_LP64 -DCC_PSABI -DCC_ARM64 -Icc -Icpp -Iir -o $@ $(CC_SRC_A64)

build/skj-cc-rv: $(CC_SRC_RV) cc/cc.h cpp/cpp.h cpp/internal.h ir/ir.h | build
	$(CC) $(CFLAGS) -Icc -Icpp -Iir -o $@ $(CC_SRC_RV)

## ColdFire assembler
AS_SRC := as/main.c as/lex.c as/parse.c as/encode.c as/elf.c ir/util.c ir/arena.c

build/skj-as: $(AS_SRC) as/as.h | build
	$(CC) $(CFLAGS) -Ias -Iir -o $@ $(AS_SRC)

## skj-ld linker
LD_SRC := ld/main.c ld/elf_read.c ld/script.c ld/link.c ld/elf_write.c ld/mapfile.c ir/util.c ir/arena.c

build/skj-ld: $(LD_SRC) ld/ld.h ld/mapfile.h | build
	$(CC) $(CFLAGS) -Ild -Iir -o $@ $(LD_SRC)

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
VERSIONED := $(TOOLS) build/skj-tinc-x86-64 build/skj-sc-x86-64 build/skj-exc
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
SC_SRC_X86 := scheme/lex.c scheme/parse.c scheme/print.c scheme/main.c scheme/gc.c \
              scheme/lower.c $(IR_SRC) $(BE_X86)
SC_SRC_A64 := scheme/lex.c scheme/parse.c scheme/print.c scheme/main.c scheme/gc.c \
              scheme/lower.c $(IR_SRC) $(BE_A64)

build/skj-sc: $(SC_SRC) scheme/scheme.h scheme/gc.h ir/ir.h | build
	$(CC) $(CFLAGS) -Ischeme -Iir -o $@ $(SC_SRC)

build/skj-sc-rv: $(SC_SRC_RV) scheme/scheme.h scheme/gc.h ir/ir.h | build
	$(CC) $(CFLAGS) -Ischeme -Iir -o $@ $(SC_SRC_RV)

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

## MooScript compiler
MOO_SRC := moo/lex.c moo/parse.c moo/typecheck.c moo/lower.c moo/main.c $(IR_SRC) $(BE_CF)

build/skj-mooc: $(MOO_SRC) moo/moo.h ir/ir.h | build
	$(CC) $(CFLAGS) -Imoo -Iir -o $@ $(MOO_SRC)

## Excelsior compiler (reader + resolver + type checker + IR lowering,
## ColdFire backend). Lowering covers the integer / bool core so far.
EXC_SRC := excelsior/lex.c excelsior/parse.c excelsior/resolve.c \
           excelsior/typecheck.c excelsior/lower.c excelsior/main.c \
           $(IR_SRC) $(BE_CF)

build/skj-exc: $(EXC_SRC) excelsior/excelsior.h ir/ir.h | build
	$(CC) $(CFLAGS) -Iexcelsior -Iir -o $@ $(EXC_SRC)

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

## Excelsior test host (cross-compiled C)
build/exc_host.o: runtime/exc_host.c runtime/utf8.h | build
	$(M68K_CC) -std=c99 -O2 -Wall -ffreestanding -c -o $@ $<

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

check-cpp: build/skj-cpp
	@sh tests/run-cpp-tests.sh

## Excelsior end-to-end (skj-exc -> skj-as -> skj-ld -> qemu-m68k)
check-exc: build/skj-exc build/skj-as build/skj-ld build/start.o build/exc_host.o build/utf8.o
	@sh tests/run-exc-tests.sh "$(QEMU)"

## Smoke test: full in-tree toolchain (front end -> skj-as -> skj-ld -> qemu)
check-smoke: build/skj-tinc build/skj-sc build/skj-mooc build/skj-cc build/skj-as build/skj-ld
	@sh tests/smoke.sh "$(QEMU)"

## skj-as validation: assemble everything with skj-as instead of m68k-linux-gnu-as
check-as: build/skj-tinc build/skj-sc build/skj-mooc build/skj-pc build/skj-as
	@sh tests/check-as.sh "$(QEMU)"

## Full qemu matrix across every supported architecture: the per-language
## suites, plus the IR-builder integration tests (fpu / i64 / unsigned ops).
## ColdFire (qemu-m68k), RISC-V (qemu-riscv32, hardware float), x86-32
## (qemu-i386), AArch64 (qemu-aarch64).
check-all: check check-cc check-cc-x86-64 check-cc-arm64 check-cc-rv check-cpp check-exc \
           check-rv check-x86 check-x86-64 check-arm64 \
           test-gc \
           test-fpu test-fpu-rv test-fpu-x86 test-fpu-x86-64 test-fpu-arm64 \
           test-i64 test-i64-rv test-i64-x86 test-i64-x86-64 test-i64-arm64 \
           test-ops test-ops-rv test-ops-x86 test-ops-x86-64 test-ops-arm64
	@echo "All architecture test suites passed."

clean:
	rm -rf build

.PHONY: all release-check check check-rv check-x86 check-x86-64 check-arm64 check-all check-cc check-cc-x86-64 check-cc-arm64 check-cc-rv check-cpp check-exc check-as check-smoke test-gc test-fpu test-fpu-rv test-fpu-x86 test-fpu-x86-64 test-fpu-arm64 test-f32-rv test-i64 test-i64-rv test-i64-x86 test-i64-x86-64 test-i64-arm64 test-ops test-ops-rv test-ops-x86 test-ops-x86-64 test-ops-arm64 test-parse clean FORCE
