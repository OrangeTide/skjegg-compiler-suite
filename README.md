# Skjegg

Skjegg is a retargetable compiler toolkit: a shared 3AC intermediate
representation, multiple backends, and multiple language front ends.
It favors implementation simplicity over sophisticated optimization,
trading a small effort for a big payoff.

## Tools

Ahead-of-time compilers (lower source to target assembly):

| Tool       | Description                                     | Status  |
|------------|-------------------------------------------------|---------|
| `skj-tinc` | TinC compiler (minimal C)                       | Working |
| `skj-sc`   | TinScheme compiler                              | Working |
| `skj-mooc` | MooScript compiler                              | Working |
| `skj-pc`   | Pascal compiler                         | Working |
| `skj-kpc`  | Kobold compiler (Pascal-derived; per target)    | Working |
| `skj-cc`   | C compiler (C89 + C99 subset)                   | Working |
| `skj-cpp`  | C preprocessor (library, also used by `cc`)     | Working |

In-process JITs (compile to machine code in memory and run it, no external
assembler):

| Tool         | Description                                  | Status  |
|--------------|----------------------------------------------|---------|
| `skj-jit`    | C JIT (x86-64 host; also `-arm64`, `-rv`)    | Working |
| `skj-kp-jit` | Kobold JIT (x86-64 host; also `-arm64`, `-rv`)| Working |

Binutils-free assemblers, linkers, and archive tool:

| Tool                     | Description                               | Status  |
|--------------------------|-------------------------------------------|---------|
| `skj-as`, `skj-ld`       | ColdFire/m68k assembler + linker (ELF32)  | Working |
| `skj-as-rv`, `skj-ld-rv` | RISC-V RV32 assembler + linker            | Working |
| `skj-as-mips`, `skj-ld-mips` | MIPS I assembler + linker (ELF and PlayStation PS-EXE) | Working |
| `skj-as-x86-64`, `skj-ld-x86-64` | x86-64 assembler + linker (ELF64)  | Working |
| `skj-as-aarch64`, `skj-ld-aarch64` | AArch64 assembler + linker (ELF64 and Mach-O) | Working |
| `skj-ar`                 | `ar(1)` archive tool (any ELF target)     | Working |

Runner, sandbox, and debugging:

| Tool            | Description                                             | Status  |
|-----------------|---------------------------------------------------------|---------|
| `skj-run`       | Sandboxed guest runner: emulates ColdFire/RV32/MIPS, runs x86-64/arm64 natively under a hypervisor | Working |
| `skj-dbg`       | Source-level debugger (DWARF line info, backtraces)     | Working |
| `skj-addr2line` | Address to source line for skj targets                  | Working |
| `skj-mkromfs`   | Build a read-only romfs image for the sandbox filesystem | Working |

Most compilers exist in per-backend variants (for example `skj-cc-rv`,
`skj-cc-x86-64`, `skj-cc-arm64`).  The assembler and linker family is a
binutils-free replacement for the ColdFire, RISC-V, MIPS, x86-64, and
AArch64 targets, verified against GNU as and the qemu suites.

## Architecture

```
front ends              shared IR              backends
──────────              ─────────              ────────
tinc/    ──┐                               ┌── backend/cf_emit.c     (ColdFire/m68k)
scheme/  ──┤                               │
moo/     ──┼──▶  ir/ir.h  ──▶  regalloc  ──┼── backend/rv_emit.c     (RISC-V RV32IMFD+Zfh)
pascal/  ──┤       │                       │
cc/      ──┘       │                       ├── backend/x86_emit.c    (x86-64)
  ▲                │                       │
  │                └── ir/ir.c, ir/util.c  ├── backend/arm64_emit.c  (AArch64)
  │                                        │
cpp/  (preprocessor library)               └── backend/mips_emit.c   (MIPS I / R2000/R3000)
```

Each front end lowers source to the same IR (defined in `ir/ir.h`).
The IR feeds a target-specific register allocator and code emitter.
Adding a new front end means writing `lex.c`, `parse.c`, `lower.c`, and
`main.c`; the IR and backends are reused unchanged.  Adding a new
backend means writing a regalloc + emitter pair and a
`runtime/start_*.S`.

Not every front end targets every backend.  TinC and TinScheme run on
every backend.  The C compiler runs on ColdFire, RISC-V, x86-64,
AArch64, and MIPS.  Pascal builds for ColdFire, x86-64, AArch64, and
MIPS, and its Kobold dialect (via `skj-kpc` and the Kobold JIT) also
targets RISC-V.  MooScript is ColdFire only.  The genuine 32-bit ISAs
(ColdFire, RISC-V RV32, MIPS) are ILP32; x86-64 and AArch64 are LP64,
the pointer-width axis living in `ir/target.h`.

## Sandbox emulator and hypervisor

`skj-run` runs a compiled guest in-tree the way `qemu-user` does, but it
is both an emulator and a thin hypervisor. The 32-bit cores (ColdFire,
RISC-V RV32, MIPS I) run by interpretation; 64-bit guests (x86-64,
AArch64) run natively through a hardware hypervisor, KVM on Linux and
HVF on macOS. It is the only runner for strict ColdFire code, which qemu
does not model.

Every guest runs inside a sandbox: one process per guest, a
capability-based filesystem of named mounts confined with openat2
`RESOLVE_BENEATH`, a small unix-like process model with spawn/wait/kill,
a `jsh` shell and a set of coreutils, a host-served read-only `proc:`
pseudo-filesystem, and a gdb stub for remote debugging. See
`doc/tools/emulator.md` and `doc/tools/process-model.md`.

## C standard library

`libc/` is a small, freestanding-friendly C standard library shared
across all five targets (ColdFire, RISC-V, MIPS, x86-64, AArch64). It
provides the headers plus string/ctype, the `snprintf`/`printf` family
with floating conversions, the stdlib numerics and the `malloc` family
over a single `__libc_sbrk` seam, buffered stdio, `errno`,
`strtod`/`sscanf`, and `atexit` exit-time flushing. It factors out
runtime pieces that each consumer used to duplicate (the 64-bit integer
helpers, the bump arena, integer/float formatting). Built and checked
with `make check-libc` and the per-target `check-libc-*`; see
`doc/tools/libc.md`.

## JIT runtime

The in-process JITs (`skj-jit` for C, `skj-kp-jit` for Kobold) compile a
source to native machine code in the running process and call into it,
rather than writing assembly for an external assembler. Each drives the
same instruction selector as the ahead-of-time backend through a shared
sink interface: one selector, two sinks, a text sink emitting GAS
assembly for the AOT path and a byte sink emitting raw machine code for
the JIT. Hosts are x86-64 and AArch64, plus an RV32 variant whose emitted
guest code runs under the emulator. See `doc/tools/jit.md`.

## PlayStation SDK

`libpsx` is a runtime SDK for writing original-PlayStation (R3051) games in C,
Pascal, and Kobold. It drives the console hardware: the GPU and
double-buffered display, DMA, sprites and textures, the SPU with ADSR and
reverb, CD-DA and XA audio, the controller and memory card, the three timers,
16.16 fixed-point math, debug text, and 3D through the GTE and an ordering
table. It builds on the MIPS backend and the PS-EXE output mode, boots on a
retail BIOS, and is tested headlessly on the PCSX-Redux emulator by framebuffer
and audio capture. Pascal and Kobold reach the C library through thin `psx`
units. See `doc/tools/psx-sdk.md` for the architecture and
`doc/tools/psx-api.md` for the full function reference.

## Layout

```
ir/          portable IR library (types, opcodes, builder)
backend/     code generation: ColdFire, RISC-V RV32, x86-64, AArch64, MIPS I
as/          ColdFire assembler, plus the RV32, MIPS, x86-64, and AArch64 front ends over a shared skeleton
ld/          ColdFire linker, plus the RV32, MIPS, x86-64, and AArch64 front ends over a shared layout core
ar/          target-neutral ar(1) archive tool
cpp/         C preprocessor (library and standalone tool)
emu/         emulator cores (ColdFire, RV32, MIPS), the KVM/HVF hypervisor, and skj-run (doc/tools/emulator.md)
jit/         in-process JIT byte sinks and arena (doc/tools/jit.md)
libc/        shared C standard library (doc/tools/libc.md)
runtime/     target runtimes (start_*.S per target, pascal_rt.c, str.c, list.c, 64-bit helpers)
tinc/        TinC front end (C-like)
scheme/      TinScheme front end (Scheme subset with GC; doc/internals/closures.md)
moo/         MooScript front end
pascal/      Pascal front end (doc/languages/pascal.md)
cc/          C front end (uses the cpp library)
libpsx/      PlayStation SDK (doc/tools/psx-sdk.md, psx-api.md)
units/       language units over libpsx (psx.pas, psx.kp)
tests/       test programs and harnesses
doc/         design and reference docs (tools/, languages/, internals/)
```

## Documentation

The reference manual is published at
<https://orangetide.github.io/skjegg-compiler-suite/>, organized by language,
tool, and internals. Its source of truth is the Markdown under `doc/`:

- `doc/languages/` the language front ends, including the full Kobold-Pascal
  specification under `doc/languages/kobold/` (grammar, language reference,
  units, module format, host ABI, and more).
- `doc/tools/` the emulator, the JIT, the libc, the MIPS backend, and the
  PlayStation SDK (`psx-sdk.md` and the `psx-api.md` function reference).
- `doc/internals/` the shared IR, calling conventions, closures, and the LP64
  model.

Build the manual locally with `make -C doc` (HTML) or `make -C doc all` (HTML
and PDF); it needs `pandoc`, and PDF output additionally needs `typst`. The
built site lands in `build/manual/`, and `build/manual/index.html` opens
directly in a browser.

## Prerequisites

- A host C compiler (`cc`, C99).
- `m68k-linux-gnu-gcc` (Debian/Ubuntu: `gcc-m68k-linux-gnu`).
  `m68k-linux-gnu-as` and `m68k-linux-gnu-ld` are optional, since
  `skj-as` and `skj-ld` can replace them.
- `qemu-user` for the user-mode emulators (`qemu-m68k`, `qemu-riscv32`,
  `qemu-x86_64`, `qemu-aarch64`, `qemu-mipsel`).  `skj-run` covers the
  32-bit targets without qemu, and runs 64-bit guests natively under a
  hypervisor (KVM on Linux, HVF on macOS).

Per-target toolchains, needed only to build and test that target:

- RISC-V: `binutils-riscv64-linux-gnu` (and `gcc-riscv64-linux-gnu` for
  a gcc-built guest runtime).  `skj-as-rv`/`skj-ld-rv` can replace binutils.
- AArch64: `gcc-aarch64-linux-gnu`, `binutils-aarch64-linux-gnu`.
- MIPS: `binutils-mipsel-linux-gnu` (or a bare-metal `mipsel-none-elf`
  cross).  `skj-as-mips`/`skj-ld-mips` can replace binutils.
- x86-64: no host binutils needed; `skj-as-x86-64`/`skj-ld-x86-64` are
  the in-tree assembler and linker.  KVM (`/dev/kvm`) is needed to run
  x86-64 guests under `skj-run`.

## Build and Run

```sh
make              # build all tools (every front end, backend, and toolchain)
make check        # ColdFire suite: compile, assemble, link, run under qemu-m68k
make check-rv     # RISC-V RV32 suite under qemu-riscv32
make check-x86-64 # x86-64 suite under qemu-x86_64
make check-arm64  # AArch64 suite under qemu-aarch64
make check-cc     # C compiler tests (skj-cc -> skj-as -> skj-ld -> qemu-m68k)
make check-cpp    # C preprocessor tests
make check-libc   # C standard library tests across every target
make check-jit    # in-process JIT (skj-jit) tests
make check-emu    # skj-run emulator and sandbox tests
make check-rvas   # RV32 assembler/linker golden-master vs GNU as
make check-all    # the whole qemu matrix across every backend
make check-smoke  # full in-tree toolchain smoke test (no cross toolchain needed)
make clean        # remove build/
```

`make help` prints the full target set (per-backend suites, the
emulator tiers, the MIPS and PlayStation paths, and the integer/float
integration tests).  `make check-smoke` exercises the full in-tree
pipeline (front end -> `skj-as` -> `skj-ld` -> `qemu-m68k`) without
requiring `m68k-linux-gnu-as` or `m68k-linux-gnu-ld`.

## Using the Tools Directly

```sh
# TinC for ColdFire, using the in-tree assembler and linker
./build/skj-tinc -o out.s tests/bsearch.tc
./build/skj-as -o out.o out.s
./build/skj-as -o start.o runtime/start.S
./build/skj-ld -o prog start.o out.o
qemu-m68k ./prog ; echo $?

# C for RISC-V RV32, using the in-tree RV toolchain
./build/skj-cc-rv -o out.s tests/cc_t001_return.c
./build/skj-as-rv -o out.o out.s
./build/skj-as-rv -o start.o runtime/start_rv.S
./build/skj-ld-rv -o prog start.o out.o
qemu-riscv32 ./prog ; echo $?
```

## Vendoring

`vendor.sh` copies selected Skjegg components into another project.
The IR core is always included.  Selection is along three axes
(comma- or space-separated, repeatable): `--arch` picks target
architectures, `--frontend` picks language front ends, and `--with`
picks per-target or standalone features.  Each axis expands into the
flat component tokens, which are still accepted directly so an older
`update-skjegg.sh` keeps working.

```sh
# C for RISC-V with the in-tree toolchain and the skj-run emulator
./vendor.sh --arch riscv --frontend cc --with toolchain,emulator

# A self-contained 64-bit C target running natively under the hypervisor
./vendor.sh --arch x86-64 --frontend cc --with toolchain,runtime,emulator

# TinC + Pascal for ColdFire with the assembler (flat-component form)
./vendor.sh coldfire tinc pascal as

# Vendor into a custom directory
./vendor.sh -d lib/skjegg --arch coldfire --frontend tinc
```

The `--with` features are `toolchain` (the in-tree assembler, linker,
and `ar` for each selected `--arch`), `emulator` (the `skj-run` core per
arch: an emulated CPU for coldfire/riscv/mips, the KVM/HVF
hardware-virtualization core for x86-64/aarch64), `runtime` (the hosted
libc and crt), `sandbox` (build that libc with its fd I/O confined
through the capability `fs/` layer), and `cpp` (the standalone
preprocessor).

The script generates two files in the destination directory:

- **`skjegg.mk`**, an includable Makefile fragment that builds exactly
  the selected components.  Add `include skjegg/skjegg.mk` to your
  project's Makefile.
- **`update-skjegg.sh`**, a copy of `vendor.sh` with the selection baked
  in, so re-running `./skjegg/update-skjegg.sh` updates without
  repeating arguments.

The `ORIGIN` variable at the top of `vendor.sh` controls where sources
are fetched from (defaults to the GitHub repository).  Edit it to point
at a fork or local path.

### Flat components

The axes expand to these tokens, which may also be passed directly.

| Name       | What it vendors                                          |
|------------|----------------------------------------------------------|
| `coldfire` | ColdFire/m68k backend + `runtime/start.S`                |
| `riscv`    | RISC-V RV32 backend + `runtime/start_rv.S`               |
| `mips`     | MIPS I backend + runtime (incl. the soft-float path)     |
| `x86-64`   | x86-64 (LP64) backend + runtime                          |
| `arm64`    | AArch64 (LP64) backend + runtime                         |
| `tinc`     | TinC front end                                           |
| `scheme`   | TinScheme front end (with GC)                            |
| `moo`      | MooScript front end + `runtime/str.c`, `list.c`          |
| `pascal`   | Pascal front end + `runtime/pascal_rt.c`         |
| `cc`       | C compiler front end (auto-includes the cpp library)     |
| `kp`       | Kobold-Pascal AOT path (`skj-kpc`, `skj-kp`, `libkpaot.a`) |
| `as`, `ld` | ColdFire assembler and linker (library + standalone)     |
| `as-rv`, `ld-rv`     | RISC-V RV32 assembler and linker               |
| `as-mips`, `ld-mips` | MIPS assembler and linker (ld-mips writes PS-EXE) |
| `as-x86-64`, `ld-x86-64` | x86-64 ELF64 assembler and linker          |
| `as-aarch64`, `ld-arm64` | AArch64 ELF64 assembler and linker         |
| `ar`       | target-neutral `ar(1)` archive tool                      |
| `addr2line`| offline DWARF symbolizer (backtrace address to file:line) |
| `cpp`      | C preprocessor (library + standalone tool)               |
| `emu-cf`, `emu-rv`, `emu-mips` | the ColdFire, RV32, and MIPS emulator cores |

A front-end selection needs at least one backend.  The `cc` component
pulls in the `cpp` library automatically.

## Name

Norwegian: *skjegg* (beard).  From the idiom *a sta med skjegget i
postkassa*, to get your beard stuck in the mailbox.
