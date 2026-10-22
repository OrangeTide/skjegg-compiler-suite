# The skjegg toolchain

Skjegg is a retargetable compiler toolkit. Several source languages share one
three-address intermediate representation (IR), and that IR is lowered to
several machine backends. A program written in any front end can therefore be
built for any backend with the same assembler, linker, runtime, and emulator.

This page is the map of the toolchain: the pipeline a source file travels, the
command-line tools that drive it, the targets they produce, and how to build
and run the result. The per-language references and the per-tool notes are
linked from the sections below and from the manual index.

## The pipeline

Every front end follows the same path, and the back half of that path is shared
by all of them:

```
source  ->  front end  ->  IR  ->  register allocation  ->  backend  ->  assembly
```

The front end parses the source language and lowers it to the IR. The IR is a
flat set of three-address instructions with no machine detail in it, so the
register allocator and the backend that follow are the same code for every
language. The backend emits GNU-assembler-syntax text for the chosen target.
From there an assembler produces a relocatable object and a linker produces an
executable.

Two independent choices meet in a build. The **front end** fixes the source
language. The **backend** fixes the target machine. They are orthogonal: the
same C or Pascal source compiles to ColdFire, RISC-V, x86-64, AArch64, or MIPS
by selecting a different compiler driver, with no change to the source.

The IR layer and the backend layer are described in `doc/internals/ir.md` and
`doc/internals/calling-convention.md`.

## Languages and their drivers

Each language has its own compiler, named for the language and suffixed for the
target. The ColdFire/m68k build carries the unsuffixed name; the other targets
add `-rv` (RISC-V RV32), `-x86-64`, `-arm64`, and `-mips` (with `-mips-sf` for
the soft-float PlayStation configuration where it applies).

| Language | Driver | Reference |
|----------|--------|-----------|
| C | `skj-cc` (and `-rv`, `-x86-64`, `-arm64`, `-mips`, `-mips-sf`) | `doc/languages/c.md` |
| Compact Pascal | `skj-pc` | `doc/languages/pascal.md` |
| Kobold-Pascal | `skj-kpc` (and per-target variants) | `doc/languages/kobold/` |
| TinC | `skj-tinc` (and per-target variants) | front end under `tinc/` |
| TinScheme | `skj-sc` (and per-target variants) | front end under `scheme/` |
| MooScript | `skj-mooc` | front end under `moo/` |
| Excelsior | `skj-exc` (and per-target variants) | in progress, under `excelsior/` |

C and Compact Pascal have full language references in `doc/languages/`.
Kobold-Pascal has its own manual set under `doc/languages/kobold/`, indexed from
the manual landing page. The remaining front ends are documented alongside their
sources for now.

The standalone C preprocessor is `skj-cpp`. It is also linked into `skj-cc` as a
library, so a C build preprocesses without a separate step. See the preprocessor
section of `doc/languages/c.md`.

### Compiling and linking a program

A compiler driver reads one source file and writes one assembly file:

```sh
skj-cc -o hello.s hello.c
```

For C there is also a single driver, `skjcc`, that runs the whole chain
(preprocess, compile, assemble, link against the freestanding libc) and writes
an executable. It links the libc archive pull-only, so an object is pulled from
the archive only when a symbol in it is referenced.

The other languages run the stages explicitly: compile to assembly with the
front end, assemble with `skj-as`, then link with `skj-ld`. The manual example
for Compact Pascal in the repository `CLAUDE.md` shows the full sequence.

## Assembler, linker, and archiver

The in-tree assembler and linker target ColdFire/m68k. They read the backend's
GNU-assembler syntax and produce big-endian ELF32, and they replace the external
`m68k-linux-gnu` tools for that target.

- **`skj-as`** is the ColdFire assembler. It reads the GAS-syntax subset the
  backends emit and produces ELF32 big-endian relocatable objects. A two-pass
  design collects labels and sizes first, then emits bytes and resolves
  branches. The companion assemblers `skj-as-rv` and `skj-as-mips` serve the
  RISC-V and MIPS targets.
- **`skj-ld`** is the ColdFire linker. It reads relocatable objects, parses a
  subset of GNU linker-script syntax (`MEMORY`, `PHDRS`, `SECTIONS`, symbol
  assignments, `/DISCARD/`), and writes executable ELF32. The default built-in
  script targets the emulator's user-mode layout. `skj-ld-rv` and `skj-ld-mips`
  serve the other targets.
- **`skj-ar`** is the archiver that builds the `libc.a` and runtime archives.

For the x86-64 and AArch64 targets the toolchain uses the platform's own
assembler and linker; for MIPS and RISC-V the `skj-as-*`/`skj-ld-*` tools and the
external toolchains are both available. The assembler's instruction coverage for
ColdFire is inventoried in `as/INSTRUCTIONS.md`.

## Running and debugging

- **`skj-run`** is the emulator. It executes a linked guest, maps guest system
  calls to the host, and serves as the reference run environment for the
  RISC-V and ColdFire tiers. It also hosts the source-level debugger. See
  `doc/tools/emulator.md`.
- **`skj-dbg`** and **`skj-addr2line`** are the debugging front ends. The
  debugger speaks the GDB remote-serial protocol to a stub inside `skj-run`, so
  a stock `gdb` or the in-tree client both attach. Symbolization reads DWARF 4
  emitted into the output ELF. See `doc/tools/debugger.md`.
- The **JIT** drivers (`skj-jit`, `skj-jit-rv`, `skj-jit-arm64`, and the Kobold
  and Excelsior JITs) execute the IR in process instead of emitting an object.
  See `doc/tools/jit.md`.

qemu (`qemu-m68k`, `qemu-riscv32`, `qemu-i386`, `qemu-x86_64`, `qemu-aarch64`)
is the independent cross-check for every target and the default run environment
in the test suite.

## Targets and ABIs

Code generation covers five backends. Each external call uses that target's
platform ABI, so an object links against code built by the native toolchain.

| Target | Register model | Integer ABI | Float |
|--------|----------------|-------------|-------|
| ColdFire / m68k | big-endian, ILP32 | m68k C ABI | hardware FPU |
| RISC-V RV32 | little-endian, ILP32 | RISC-V ILP32 psABI | soft-float ABI, hardware FD+Zfh |
| MIPS I (o32) | configurable endian, ILP32 | MIPS o32 psABI | hardware FPU, or soft-float (`-mips-sf`) |
| x86-64 | little-endian, LP64 | SysV | SSE |
| AArch64 | little-endian, LP64 | SysV (AAPCS64) | hardware `d` registers |

The 32-bit targets are ILP32 (`int`, `long`, and pointer all 32 bits). The
64-bit targets are LP64 but run in a low-memory, 32-bit-address model, so a
pointer occupies eight bytes yet every address fits in 32 bits. The data model
is documented in `doc/internals/lp64.md`, and the per-call conventions in
`doc/internals/calling-convention.md`.

A **freestanding** tier (ColdFire and RISC-V) links against the in-tree runtime
and libc with no host operating system, which is what the ColdFire and RISC-V
virtual-machine hosts run. The x86-64, AArch64, and MIPS targets are hosted
development and test configurations. The freestanding runtime, its shared system
call seam, and the libc are described in `doc/tools/libc.md` and `doc/abi/`.

## Building

The top-level `Makefile` builds every tool and runs the test suites. The common
targets:

```sh
make                # build all tools: compilers, assembler, linker, backends
make check          # compile, assemble, link, and run the tests under qemu-m68k
make check-rv       # the same for RISC-V RV32 under qemu-riscv32
make check-x86-64   # the same for x86-64 under qemu-x86_64
make check-arm64    # the same for AArch64 under qemu-aarch64
make check-all      # every architecture's qemu suite plus the integration tests
make clean          # remove build/
```

Per-language and per-feature suites (`check-cc`, `check-cpp`, `check-exc`, the
`test-fpu`/`test-i64`/`test-ops` integration tests, and the Kobold suites) are
listed with the rest in `CLAUDE.md`. Every binary is built through a Makefile
target; the test runner only runs a `build/<name>` that has a matching source or
fixture, so a stray build is skipped rather than reported.

The cross toolchain the build expects is `m68k-linux-gnu-gcc`,
`riscv64-linux-gnu-{as,ld}`, `nasm`, and the matching `qemu-*` runners. The
ColdFire `m68k-linux-gnu-as` and `-ld` are optional, since `skj-as` and `skj-ld`
replace them.

## Vendoring and versioning

`vendor.sh` copies selected components into a consumer project. It generates
`skjegg.mk`, an includable Makefile fragment, and `update-skjegg.sh`, a
re-vendoring script with the component selection baked in. The preprocessor and
assembler components split into a library and a standalone `main.c` so other
tools can reuse them, and the C front end pulls in the preprocessor library
automatically.

`version.mk` holds `SKJ_VERSION` and is the source of truth for the release
number. An in-tree build refines it from `git describe`; a vendored build uses
the version string baked in at vendoring time, never `git describe`, since git
in the consumer would report the consumer's own tags. Releasing is two steps:
bump `version.mk`, then tag the commit `v<version>`. `make release-check`
verifies the tag exists, sits at HEAD, and the tree is clean.

## Where to go next

- A language: `doc/languages/c.md`, `doc/languages/pascal.md`, or the
  Kobold-Pascal set under `doc/languages/kobold/`.
- The tools: `doc/tools/emulator.md`, `doc/tools/debugger.md`,
  `doc/tools/jit.md`, `doc/tools/libc.md`, and `doc/tools/mips.md`.
- The internals: `doc/internals/ir.md`,
  `doc/internals/calling-convention.md`, `doc/internals/lp64.md`,
  `doc/internals/closures.md`, and `doc/internals/kobold-heap.md`.
