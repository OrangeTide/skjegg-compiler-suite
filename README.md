# Skjegg

Skjegg is a retargetable compiler toolkit: a shared 3AC intermediate
representation, multiple backends, and multiple language front ends.
It favors implementation simplicity over sophisticated optimization,
trading a small effort for a big payoff.

## Tools

| Tool       | Description                | Status  |
|------------|----------------------------|---------|
| `skj-tinc` | TinC compiler (minimal C)  | Working |
| `skj-sc`   | TinScheme compiler         | Working |
| `skj-mooc` | MooScript compiler         | Working |
| `skj-pc`   | Compact Pascal compiler    | Working |
| `skj-cc`   | C compiler (C89+C99 subset)| Working |
| `skj-cpp`  | C preprocessor             | Working |
| `skj-as`   | ColdFire/m68k assembler    | Working |
| `skj-ld`   | ColdFire/m68k static linker| Working |

## Architecture

```
front ends              shared IR              backends
──────────              ─────────              ────────
tinc/    ──┐                               ┌── backend/cf_emit.c    (ColdFire/m68k)
scheme/  ──┼──▶  ir/ir.h  ──▶  regalloc  ──┤
moo/     ──┤       │                       └── backend/rv_emit.c    (RISC-V RV32IM)
pascal/  ──┘       │
                   └── ir/ir.c, ir/util.c
```

Each front end lowers source to the same IR (defined in `ir/ir.h`).
The IR feeds into a target-specific register allocator and code
emitter.  Adding a new front end means writing `lex.c`, `parse.c`,
`lower.c`, and `main.c` -- the IR and backend are reused unchanged.
Adding a new backend means writing a regalloc + emitter pair and a
`runtime/start_*.S`.

## Layout

```
ir/          portable IR library (types, opcodes, builder)
backend/     target-specific code generation (ColdFire/m68k, RISC-V RV32IM)
as/          ColdFire/m68k assembler (GAS syntax subset -> ELF32)
ld/          ColdFire/m68k static linker (ELF32, ld script subset)
runtime/     target runtimes (start.S, pascal_rt.c, rope.c, list.c, 64-bit helpers)
tinc/        TinC front end (C-like)
scheme/      TinScheme front end (Scheme subset with GC)
moo/         MooScript front end
pascal/      Compact Pascal front end
tests/       test programs and harness
```

## Prerequisites

- A host C compiler (`cc`, C99).
- `m68k-linux-gnu-gcc`
  (Debian/Ubuntu: `gcc-m68k-linux-gnu`).
  `m68k-linux-gnu-as` and `m68k-linux-gnu-ld` are optional --
  `skj-as` and `skj-ld` can replace them.
- `riscv64-linux-gnu-as` and `riscv64-linux-gnu-ld`
  (Debian/Ubuntu: `binutils-riscv64-linux-gnu`).
- `qemu-m68k` and `qemu-riscv32` user-mode emulators
  (Debian/Ubuntu: `qemu-user`).

## Build and Run

```sh
make              # build all tools (compilers + assembler + RISC-V variants)
make check        # compile tests, assemble, link, run under qemu-m68k
make check-rv     # same for RISC-V RV32IM under qemu-riscv32
make check-as     # same as check, but using skj-as instead of m68k-linux-gnu-as
make check-cc     # C compiler tests (skj-cc -> skj-as -> skj-ld -> qemu-m68k)
make check-cpp    # C preprocessor tests (macro expansion, conditionals, etc.)
make check-smoke  # full in-tree toolchain smoke test (no cross toolchain needed)
make test-gc      # TinScheme GC unit tests (host-only)
make test-fpu     # FPU integration test (IR builder -> ColdFire asm -> qemu-m68k)
make test-i64     # 64-bit integer test on ColdFire (IR builder -> qemu-m68k)
make test-i64-rv  # 64-bit integer test on RISC-V (IR builder -> qemu-riscv32)
make test-ops     # unsigned 32-bit ops test on ColdFire (IR builder -> qemu-m68k)
make test-ops-rv  # unsigned 32-bit ops test on RISC-V (IR builder -> qemu-riscv32)
make clean        # remove build/
```

`make check-smoke` exercises the full in-tree toolchain pipeline
(front end -> `skj-as` -> `skj-ld` -> `qemu-m68k`) without requiring
`m68k-linux-gnu-as` or `m68k-linux-gnu-ld`.

## Using the Tools Directly

```sh
# TinC example (using skj-as)
./build/skj-tinc -o out.s tests/bsearch.tc
./build/skj-as -o out.o out.s
./build/skj-as -o start.o runtime/start.S
m68k-linux-gnu-ld -o prog start.o out.o
qemu-m68k ./prog ; echo $?

# Pascal example (using skj-as)
./build/skj-pc -o out.s tests/pascal_t042_string_assign.pas
./build/skj-as -o out.o out.s
./build/skj-as -o start.o runtime/start.S
m68k-linux-gnu-gcc -std=c99 -O2 -Wall -ffreestanding -c -o pascal_rt.o runtime/pascal_rt.c
m68k-linux-gnu-ld -o prog start.o pascal_rt.o out.o
qemu-m68k ./prog ; echo $?
```

## Vendoring

`vendor.sh` copies selected Skjegg components into another project.
Pick backends (`coldfire`, `riscv`) and front ends (`tinc`, `scheme`,
`moo`, `pascal`, `cc`) plus optional tools (`as`, `cpp`).  The IR
core is always included.

```sh
# Vendor TinC + Pascal compilers with ColdFire backend and assembler
./vendor.sh coldfire tinc pascal as

# Vendor just the C compiler (pulls in cpp library automatically)
./vendor.sh coldfire cc

# Vendor into a custom directory
./vendor.sh -d lib/skjegg coldfire tinc
```

The script generates two files in the destination directory:

- **`skjegg.mk`** -- includable Makefile fragment that builds exactly
  the selected components.  Add `include skjegg/skjegg.mk` to your
  project's Makefile.
- **`update-skjegg.sh`** -- a copy of `vendor.sh` with the selected
  components baked in, so re-running `./skjegg/update-skjegg.sh`
  updates without repeating arguments.

The `ORIGIN` variable at the top of `vendor.sh` controls where sources
are fetched from (defaults to the GitHub repository).  Edit it to
point at a fork or local path.

### Components

| Name       | What it vendors                                        |
|------------|--------------------------------------------------------|
| `coldfire` | ColdFire/m68k backend + `runtime/start.S`              |
| `riscv`    | RISC-V RV32IM backend + `runtime/start_rv.S`           |
| `tinc`     | TinC front end                                         |
| `scheme`   | TinScheme front end (with GC)                          |
| `moo`      | MooScript front end + `runtime/rope.c`, `list.c`       |
| `pascal`   | Compact Pascal front end + `runtime/pascal_rt.c`       |
| `cc`       | C compiler front end (auto-includes cpp library)       |
| `as`       | ColdFire/m68k assembler (library + standalone tool)    |
| `ld`       | ColdFire/m68k static linker (library + standalone tool)|
| `cpp`      | C preprocessor (library + standalone tool)             |

Front ends require at least one backend.  The `cc` component
automatically pulls in the `cpp` library files.

## Name

Norwegian: *skjegg* (beard).  From the idiom *a sta med skjegget i
postkassa* -- to get your beard stuck in the mailbox.
