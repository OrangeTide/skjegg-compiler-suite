# Changelog

All notable changes to this project are recorded here, newest first, grouped by
month. Releases up to and including 0.12.0 are reconstructed from their git tag
notes; the detail there is shallower than for recent releases.

## October 2026

### 0.16.0

#### Added
- In-guest self-hosting: the whole skj toolchain (skj-cc, skj-as, skj-ld, and
  skj-kpc, the Kobold compiler) builds and runs as guest ELFs under skj-run on
  RV32 and ColdFire. The Kobold compiler self-compiles in-guest and its output
  matches the host byte for byte. New opt-in `make` targets prove each stage.
- skj-ld reads objects without mmap under the guest libc, and fchmodat/chmod
  were added to the emulator and the libc so the linker runs in-guest.

#### Changed
- The compiler does about 3.8x less work when self-compiling, with byte-identical
  output. EmittedRoutineName's overload scan is resolved once per routine instead
  of per call site, LowerStr builds its result in place, symbol comparisons use
  the precomputed lowercase name, and a no-allocation SameTextCI serves the
  parser's case-insensitive checks.
- skj-run caches store write permission in a page directory parallel to the main
  one, so a store to a committed writable page skips the per-access region check.

#### Fixed
- skj-cpp collects function-like macro arguments split across source lines.
- skj-as: a use-after-free in the lexer-rewind peek.

### 0.15.0

#### Added
- Kobold standard-library units with dotted namespaces (the core, io, std,
  experimental, and kobold tiers) and a `uses ns.*` wildcard. Runtime services
  (RNG, clock, sleep, console I/O, files) are now reached through units rather
  than compiler builtins. Adds the core.strings unit and a typed read/write
  surface for io.console.
- boris runtime glue (A1-A7): a freestanding heap backed by brk, a 32-bit
  xoshiro128** RNG with seed-driven and pure generators, a clock_gettime and
  getrandom sandbox seam, Sleep/Yield over nanosleep, a sticky EOF latch for the
  freestanding console, and the guest trap ecall aligned with boris VM_SYS_TRAP.

#### Changed
- Strings carry a byte capacity and grow in place: the self-append idiom
  `s := s + e` and left-leaning concat chains lower to amortized in-place appends
  instead of allocating and copying the whole left operand each step.
- A one-character string literal is accepted where a char argument is expected.

#### Tooling
- `check-kp-corpus-san`: the Kobold corpus under the GH_POISON guest-heap
  instrument plus ASan and UBSan.

### 0.14.0

#### Added
- Guest file I/O: openat and real lseek in the sandbox; whole-file and
  line/stream handles through a Kobold `files` unit; stat/fstat via statx across
  rv32, ColdFire, MIPS, x86-64, and arm64; directory reads via getdents64 with a
  `dirs` unit; and mkdir, rmdir, unlink, rename, and remove over the *at syscalls.
- One buffered-stdio core and one syscall seam shared between the C libc and the
  Kobold runtime; open/close/lseek on every C crt; open_memstream; and sbrk
  backed by real brk on ILP32 so a full translation unit self-hosts.
- DWARF for ColdFire and MIPS, with the serializer and reader parameterized by
  endianness; abort and assert are symbolized like a fault.
- skj-cc self-hosts under skj-run on RV32, filling the libc and runtime gaps it
  hit, and `-h`/`--help` is recognized uniformly across every skj-* tool.

#### Changed
- skj-as defaults to 32-bit long-form branches, with `-mshort-branches` to opt
  out, and gains an `.include` directive.

#### Fixed
- A Kobold string release-after-free in kpcom's IsOwnedExpr, found with the new
  GH_POISON instrument.
- cc/backends: a `%ld`/int64 vararg bug and continue inside a switch.

### 0.13.0

#### Added
- A source-level debugger for the toolchain. Source position is carried through
  the IR for `-g` across every front end (C, Pascal, MooScript, TinC, Excelsior,
  Kobold). A DWARF 4 `.debug_line`/`.debug_info`/`.debug_abbrev` serializer;
  skj-as-rv and skj-ld-rv build, carry, and relocate the `.debug_*` sections and
  emit sized STT_FUNC symbols; a shared DWARF reader with skj-addr2line; a
  zero-allocation frame-pointer backtrace that skj-run uses to symbolize faults;
  a GDB remote-protocol stub in skj-run (`--gdb PORT`); and skj-dbg, an
  interactive source-level debugger over that stub.

#### Changed
- Vendored trees copy the usage docs for each selected tool and language.
- CI runners are pinned to ubuntu-24.04.

### 0.12.0

#### Added
- Freestanding RISC-V guest runtime that builds with the skj toolchain alone:
  skj-as-rv gains the tail pseudo and skj-ld-rv resolves it, so no binutils or
  external riscv32 gcc is needed.
- Linker map output (`-Map`) for skj-ld, skj-ld-rv, and skj-ld-mips, listing
  memory regions, output sections with input contributions, and symbols by
  address.
- Kobold dialect features: properties, failable methods with wide results (real,
  int64, record), a set as a dynamic-array element and a map value, real and
  single-precision constants, and string case selectors.
- Near clean-room Kobold specification: static and dynamic semantics, a built-in
  routine signature reference, the host ABI across all four backends, and the
  KPM1 fingerprint and IR opcode reference.

#### Changed
- skj-cc models const qualifiers in its type system, distinguishing
  `int * const` from `const int *`, and rejects a braced scalar initializer with
  the wrong element count.
- Const globals and static-const locals are placed in `.rodata` on all five
  backends. skj-as gains a `.rodata` section for ColdFire, and an uninitialized
  const stays in `.bss`.

#### Fixed
- Integer divide by zero traps cleanly on rv32 and ColdFire, and `INT_MIN / -1`
  (signed division and modulo) traps on every backend, matching the documented
  exit-70 fault semantics.
- The KPM1 reader rejects a slot-less MARK or CONT_UNWIND instead of reading out
  of bounds.

#### Tooling
- A Kobold conformance corpus pins the documented semantics and runtime fault
  exit codes. ColdFire reaches full corpus parity as a backend
  (check-kp-corpus-cf) over the hosted libkpaot-cf.a.

### 0.11.0

#### Added
- Kobold on the ColdFire VM: skj-kp-cf compiles modules ahead of time, and a
  freestanding no-libc runtime (libkpaot-cf-free.a) provides guest file I/O, a
  link script, coroutine support, and the 64-bit and transcendental helpers.
- Extended inline assembly with outputs, inputs, and clobbers on all five
  backends (ColdFire, RISC-V, x86-64, AArch64, MIPS), with integer
  register-class constraints, operand pins, and MIPS named-register pins for
  syscalls.
- Overflow-checked add, subtract, multiply, and negate in the backends, each
  with a per-site trap target. Excelsior traps on integer and decimal overflow.
- Near clean-room Kobold language reference: static and dynamic semantics, a
  built-in routine signature reference, the host ABI across all four backends,
  and the pinned KPM1 interface fingerprint.

#### Changed
- The self-hosting seed is backend-neutral and endian-neutral, so one seed
  serves the big-endian ColdFire target.
- The fails-test error binder is scoped to its branch.
- skj-run provides host file open, read, and close for guest file I/O, and
  asm_lex accepts `$` in symbol names on non-MIPS targets.

#### Fixed
- Integer divide by zero traps cleanly on rv32 and ColdFire, where it previously
  returned a wrong answer or raised a raw SIGFPE.
- The most-negative integer divided by -1 (and the matching modulo) traps
  OVERFLOW on every backend.
- A ColdFire call-argument liveness bug and a 64-bit compare bug in the register
  allocator are fixed.
- The C compiler initializes a local char array from a string literal, and the
  lexer skips GNU `__attribute__`.

#### Tooling
- New `check-kp-corpus-cf` (full Kobold corpus on ColdFire under qemu-m68k) and
  `check-kp-free-cf` (freestanding AOT path on skj-run) targets, plus a
  conformance corpus that pins value semantics and the exit-70 fault code.

### 0.10.4

#### Fixed
- skj-cpp: `__FILE__` expansion overflowed a fixed 64-byte buffer, so a long
  source path was truncated and left an unterminated string literal.
- MIPS: the crts define `__start` so the linker enters at `_start`, the runtime
  builds with the backend's `-mno-abicalls -fno-pic -mfp32` flags, and the
  encoding golden master tolerates trailing section-alignment padding.
- cc: `expect()` parse errors name the file, the tokens, and the context, while
  keeping the pinned "parse error at line" text.

#### Tooling
- CI: the mips and rv32 jobs install the toolchains they need, and the matrix
  runs on `test/**` branches for pre-release dry runs.

### 0.10.3

#### Fixed
- MIPS runtime objects build with `-mfp32`, which the Debian mipsel-linux-gnu
  cross requires.
- The rv32 lockstep and fuzz harnesses compare the full 64-bit f-register,
  including the single-precision NaN-box, now that the core is FLEN=64 like qemu.
- The fuzzer NaN-boxes its single-precision seeds so fuzzed single-precision ops
  run on real values. No core bug was involved.

### 0.10.2

#### Fixed
- tests/test_rv_fp.c NaN-boxes its single-precision operands for the FLEN=64
  f-registers.
- The instruction-count baseline is re-accepted for the intended codegen
  changes, and `exc_walker_test` builds for the gate so an accept no longer drops
  it.

#### Tooling
- The build and mips CI jobs point MIPS_CC (and, for build, MIPS_OBJCOPY) at the
  Debian mipsel-linux-gnu cross. The rv32-reference job installs the riscv cross
  its guest ELF needs.

### 0.10.1

#### Fixed
- tests/test_rv_fp.c NaN-boxes its single-precision operands for the FLEN=64
  f-registers, so the emulator suite passes. The emulator core was correct.

### 0.10.0

#### Added
- A freestanding and hosted C standard library: string, ctype, printf, stdlib, a
  boundary-tag malloc family over a `__libc_sbrk` seam, buffered stdio, errno,
  float conversions, and strtod/sscanf and related functions.
- A stackful coroutine primitive (`co_create`/`co_resume`/`co_yield`) and a
  cooperative C11 `threads.h` subset built on it. The hosted libc, coroutines,
  and threads ship vendorable with the skjcc cross driver.
- skj-cc: by-value struct passing and return on RISC-V, ColdFire, and MIPS o32,
  C99 compound literals, anonymous struct and union members, `_Bool`,
  `_Static_assert`, and `_Generic`.
- skj-cc constant initializers: float and double statics, symbol-plus-addend
  pointers, aggregate leaves, and zero-filled globals in `.bss`.
- RV32 emulator: the D (double) extension, so double-precision guests run under
  skj-run.
- Kobold: `or else` recovers managed results, and `map of T` is structurally
  compatible.
- Documentation: a C language reference, a C standard library design note, a
  Kobold guide, and the Skjegg manual as a GitHub Pages site.

#### Changed
- The in-tree linkers read `ar` archives and pull only the members that resolve
  an undefined symbol. Weak symbols are supported across the assembler and both
  linkers, and the assembler encodes `negx`.

#### Fixed
- Code generation bugs found by a cross-backend differential torture corpus
  (host gcc as oracle against every skj backend, the emulator, and the JIT),
  including unsigned and 64-bit compound assignment, a `long long` switch control
  value, ColdFire signed and unsigned modulo, and the `*p++` load width.

### 0.9.0

#### Added
- C compiler (skj-cc): variadic functions on every target through a real
  `<stdarg.h>`, with per-ABI varargs helpers. The RISC-V varargs convention is
  self-consistent for skj-to-skj calls only and does not interoperate with gcc
  callers.
- Kobold is a first-class in-tree front end (moved to `kobold/`), with `-o`
  output on both the JIT and AOT compilers and a freestanding RV32 guest runtime
  tier that has a versioned entry ABI.
- Cross-language RV32 process sandbox demo, where a C root spawns both a C child
  and a Kobold child under a shared budget.
- Fuzz harnesses for the preprocessor, the KPM1 reader, the assemblers, and the
  TinC, Pascal, MooScript, cc, and Excelsior parsers, plus a code-generator
  coverage harness and a cross-backend differential torture test.

#### Changed
- C compiler: full integer promotions and usual arithmetic conversions,
  nested-brace initializers for local aggregates with incomplete-array
  inference, string literals typed as `char[len+1]` so `sizeof` measures storage,
  and a fix for reversed multidimensional array dimensions.
- Sanitizers (ASan and UBSan) now cover every front end, the archive tool, and
  the Kobold corpora. The undefined behavior they surfaced is fixed.
- The vendored tree ships `<stdarg.h>` and bakes the default include path into
  every cc tool.
- CI runs the MIPS, emulator, RV32-reference, and conformance tiers.

### 0.8.0

#### Added
- Kobold front end: a sixth front end that reads the KPM1 text module emitted by
  its self-hosting compiler, with an in-process JIT (x86-64, plus RV32 and
  AArch64) and an ahead-of-time compiler (RV32 and AArch64). It includes a guest
  runtime for strings, dynamic arrays, maps, coroutines, and refcounted handles.
- Multi-unit ahead-of-time linking with cross-module calls through ordinary
  external linkage.
- A deterministic instruction-count benchmark in `check-emu-all`, so changes in
  generated-code size are caught.

#### Changed
- Integer overflow traps on both the Kobold JIT and AOT paths.
- The KPM1 reader validates its input, including out-of-range operand indices.
- x86-16 is dropped as out of scope, and PLAN.md is corrected.

#### Fixed
- Undefined behavior surfaced by the sanitizers is fixed, and `check-san` builds
  the toolchain under ASan and UBSan in CI.

## September 2026

### 0.7.2

#### Fixed
- CI full test matrix: the arm64 QEMU loader argument was lost because the test
  runners quoted the qemu command. The test runners now word-split `$QEMU`.

### 0.7.1

#### Fixed
- CI for the in-process JIT test tiers: the AArch64 JIT suite runs under
  qemu-aarch64 with the cross toolchain's loader, and CI installs zig to
  cross-build the RV32 JIT. No code changes.

### 0.7.0

#### Added
- In-process JITs for AArch64 (`skj-jit-arm64`) and RV32 (`skj-jit-rv`), joining
  the x86-64 JIT over a shared arch-neutral JIT core. Excelsior runs in-process
  on the AArch64 JIT, including source coroutines.
- RV32 process sandbox (`skj-run`): a round-robin scheduler with spawnable child
  processes, per-process and aggregate memory budgets, guarded dynamic memory
  (`gm_mmap`/`gm_munmap`) so coroutine-stack overruns fault, and a
  decoded-instruction cache that `fence.i` invalidates.

#### Changed
- Each backend is a single instruction selector driving a byte sink (for the JIT)
  and a text sink (for AOT output). RISC-V uses `rv_select.c` in place of
  `rv_emit.c`.
- `rv_run` returns an enum reason (budget, halt, trap, or yield) and stops early,
  which is an API change to the emu-rv component.
- Redundant license notices are removed across the tree.

#### Fixed
- Undefined behavior in the RV32 S/B/J immediate decoders, found by a new ASan
  and UBSan pass, plus hardening of `gm_mmap`/`gm_munmap` against 32-bit wrap and
  two sandbox bugs (a spawn leaving the parent's fault flag set, and a scheduler
  slice not clearing the wfi wait).

#### Tooling
- `make check-emu-san` runs the host-only emulator tests under ASan and UBSan in
  CI, with new tests for the `rv_run` contract, guest mappings, the decode cache,
  and the sandbox.
- The byte encoders are checked against GNU as (RV32) and a disassembler
  (AArch64).

### 0.6.0

#### Added
- `skj-jit`, an in-process x86-64 JIT for the C compiler. It compiles C source to
  native code in the same process and runs it, with no assembler, linker, or
  emulator. It covers the integer core, single and double float, small-struct
  by-value arguments and returns, System V varargs, and calls that spill to the
  stack.
- A new `check-jit` test tier.
- New IR opcodes: `FSQRT`, `FROUND`, 64-bit divide and modulo, overflow-checked
  `ADDO`/`SUBO`/`MULO`, and `IR_LOC` for source-location tracking.

#### Changed
- The x86-64 code generator is one instruction selector (`backend/x86_select.c`)
  with a byte sink for the JIT and a NASM text sink for ahead-of-time output.
  `skj-cc-x86-64`, `skj-exc-x86-64`, and the JIT share it.
- The x86-32 target is removed: its tools, tests, runtime, vendoring, and
  `X86_BITS==32` branches.
- RISC-V and MIPS now match the platform psABI for every external call, and the
  toolkit's stack convention is retired there. `skj-cc-rv-psabi` merges into
  `skj-cc-rv`. ColdFire keeps its convention.
- MIPS fixes: single-precision float arguments take the same eight-byte footprint
  as doubles, and a tail call whose arguments fit in registers is a real tail
  call.
- Preprocessor constraint violations now fail `skj-cpp` instead of passing
  silently.

### 0.5.1

#### Fixed
- Calls with 17 to 32 arguments compile on every backend. The per-call cap rose
  from 16 to 32 to match the C front end. Calls of 33 are still rejected by the
  front end.
- RISC-V handles stack frames larger than 2047 bytes. Out-of-range frame offsets
  go through a scratch register.
- `skj-as-rv` rejects out-of-range 12-bit immediates with a diagnostic that names
  the mnemonic and value, instead of silently truncating them.

#### Tooling
- New cross-target regression test `cc_t078`, covering a 3200-byte frame and a
  20-argument call on every C backend.

### 0.5.0

#### Added
- `skj-as-mips` and `skj-ld-mips`, a binutils-free MIPS I assembler and static
  linker for ELF32 little-endian objects. The linker applies the o32 relocations,
  emits the MIPS ABI flags, and pulls only the archive members it needs.
- `skj-ar`, a target-neutral archive tool that creates GNU/SysV archives with a
  symbol index and lists or extracts members.
- A PlayStation target. `skj-ld-mips -f ps-exe` writes a PS-X EXE image, and
  `start_psx.S` is a BIOS-TTY runtime crt. A PS-EXE test validates the container,
  and an opt-in test runs programs on PCSX-Redux, including a soft-float program
  on the FPU-less R3051. This completes MIPS milestone 6.

#### Tooling
- `check-mipsas` checks encodings against GNU as and includes a delay-slot load
  test. `check-cc-mips-skj` runs the full C suite through the in-tree toolchain
  under qemu-mipsel.
- The MIPS toolchain vendors as `as-mips`, `ld-mips`, and `ar`, plus the `mips`
  backend component and the soft-float compiler `skj-cc-mips-sf`.
- `drop-to-main` takes an explicit version and names the main commit `vX.Y.Z`.

### 0.4.0

#### Added
- A MIPS I backend for the R2000 and R3000, little-endian. It has two calling
  tiers: the toolkit's stack convention for TinC, TinScheme, and the C compiler,
  and the o32 platform ABI for Excelsior. Floating point is a build choice, and a
  `.set noreorder` mode fills delay slots with a scheduler.
- `skj-run`, an in-tree guest runner for ColdFire and RV32 that replaces
  qemu-m68k and qemu-riscv32 for those targets. It reports the guest's exit
  status, and `--trace` and `--stats` expose emulator events. The RV32 core ships
  with its audit suites.
- `skj-as-rv` and `skj-ld-rv`, an in-tree RISC-V assembler and static linker for
  ELF32 that interoperate with gcc-built code.
- Excelsior sources: the fallible pump as the iteration protocol, `source of T`
  with `yield` on a stackful coroutine, `defer`, list comprehensions, and
  function values.
- Inline assembly (`asm` and `asm volatile`) in `skj-cc`, plus a MIPS I retarget.
- Runtime additions: software float (`softfloat.c`) and 64-bit integer helpers
  (`soft64.c`) for targets without hardware support, a smolmoo runtime, and psABI
  start files for MIPS and RISC-V.

#### Changed
- The Excelsior host runtime is renamed from `exc_host` to `libexc`, with a
  public `libexc.h`.
- The ColdFire assembler's lexer and object emission are split into `asm_lex` and
  `asm_obj`.

#### Tooling
- `make check-all` passes across ColdFire, RISC-V, x86-32, x86-64, AArch64, MIPS,
  and the emulator suites, including the new RISC-V assembler and linker suites.
  Documented in `doc/mips.md` and `doc/emulator.md`.

## July 2026

### 0.3.0

#### Added
- AArch64 (ARM64) backend, and an x86-64 target sharing the x86 backend. Both are
  at feature parity with x86-32: integer core, native 64-bit integers, float,
  `_Float16`, and delimited continuations.
- Native single-precision float (IR_F32) on every backend, and RISC-V hardware
  float (RV32FD + Zfh) in place of the soft-float path.
- skj-cc retargeted to x86-64, AArch64, and RISC-V alongside ColdFire. It has an
  LP64 data model on the 64-bit targets, SysV AMD64 and AAPCS64 calling
  conventions (including varargs and struct-by-value), bit-fields, designated
  initializers, `_Alignof` and `_Alignas`, and a `_Float16` storage type.
- Excelsior (skj-exc), a new statically typed, class-based in-world scripting
  front end. It covers per-instance object state, records, enums, `set of E`,
  base-10 decimal, UTF-8 strings, lists, fallible expressions with `otherwise`
  and `on fail`, teaching runtime faults, interfaces and object slices, `shared`
  parameters, and compile-time macros with record, class, and enum introspection.
- `make release-check`, which verifies that `version.mk` and the release tag
  agree.

#### Changed
- `version.mk` is the source of truth for the version, and git only refines it. A
  vendored build reports the Skjegg version plus the commit it was vendored from,
  not the consuming project's tags.
- `long double` is a distinct, detected type instead of causing silent ABI
  corruption.

#### Fixed
- A float temp live across a call is spilled on x86 and AArch64.
- RISC-V double slots are 8-byte aligned, so `fld` and `fsd` are never
  misaligned.
- Struct and union globals are sized and aligned correctly, and pointer globals
  initialized to addresses are relocated.
- Vendored trees now get the `version.h` rule, so vendored tools compile.

## May 2026

### 0.2.0

#### Added
- The initial public toolkit: a shared 3AC IR with ColdFire/m68k and RISC-V
  backends, and a built-in ColdFire assembler, linker, and C preprocessor.
- Front ends for TinC, TinScheme, MooScript, Compact Pascal, and an early C
  compiler (skj-cc), plus `vendor.sh` for embedding components in other projects.
