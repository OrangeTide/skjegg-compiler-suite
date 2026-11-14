# Running a Kobold Pascal program in the RV32 sandbox

This is the end-to-end path from a Kobold Pascal source file to a program running
inside the in-tree RV32 emulator (`skj-run`, the `emu/rv32.c` core). It is the
freestanding tier: the program links against the no-libc guest runtime, so it is
the same build a real RV32 VM host would load, not a hosted libc executable.

`skj-kp-jit` (see `kp-guide.md`) is the quickest way to run a program on the
host, and `kobold/aot-run.sh` builds a hosted native executable. Neither loads
the program into the emulator. This document covers that path, whose steps
otherwise live only in the test runner.

## The pipeline

```
prog.pas ──(1)──▶ prog.kpm ──(2)──▶ prog.s ──(3)──▶ prog.o ──(4)──▶ prog.elf ──(5)──▶ run
       skj-kpc-rv        skj-kpc-rv      skj-as-rv       skj-ld-rv        skj-run
      (--emit-kpm)       (module→asm)    (RV32 as)      (+ crt + lib)     (rv32.c)
```

Stages 1 and 2 run `skj-kpc-rv`, the single-step Kobold compiler for RV32, natively
on the host. It needs no `fpc` or external Pascal toolchain: `skj-pc` builds it from
`kobold/kpcom.pas`, and it emits RV32 regardless of the host it runs on. Stage 1 reads
the source and emits the module; stage 2 reads that module back and lowers it to RV32
assembly. The two stages are one tool, so `skj-kpc-rv prog.pas -o prog.s` collapses
them, going straight to assembly; the module intermediate is kept here because the
sandbox test runner emits and lowers in separate steps. Stages 3 and 4 are
host-independent cross tools that emit and consume RV32 regardless of the machine they
run on. Stage 5 is the emulator.

## No fpc is involved

`fpc` is only used by `make bootstrap-fpc`, the dialect-conformance check: it
compiles `kobold/kpcom.pas` with both `fpc` and `skj-pc` to confirm the source stays
in the Pascal subset both accept, so `skj-pc` has not drifted from a mainstream
dialect. That target is never on the normal build path. The normal build compiles
`kobold/kpcom.pas` with `skj-pc` into the compiler, so no `fpc` is involved.

There is no committed seed to regenerate. After editing `kobold/kpcom.pas`, a
plain build rebuilds the front end from source and each target re-emits its own
seed. That works on any supported host; compiling your own `.pas` with the front
end works on any supported host too.

## Build the tools once

`skj-kpc-rv` runs on the host (`skj-pc` builds it from source) and targets RV32,
so it is the same build on any host. The cross toolchain and the emulator are the
same on every host too.

```sh
# the single-step RV32 compiler, the RV32 cross toolchain, freestanding runtime, emulator
make build/skj-kpc-rv \
     build/skj-as-rv \
     build/skj-ld-rv \
     build/kpfree-rv/kp_crt_rv.o \
     build/libkpaot-rv-free.a \
     build/skj-run
```

`skj-kp-jit-rv --emit-kpm` works in place of `skj-kpc-rv --emit-kpm` for stage 1,
emitting the same RV32 module from the in-process front end.

## Compile and run

```sh
# 1. source to a KPM1 module (skj-kpc-rv runs natively on the host)
build/skj-kpc-rv --emit-kpm prog.pas -o prog.kpm

# 2. KPM1 module to RV32 GAS assembly (the same compiler, reading the module back)
build/skj-kpc-rv prog.kpm -o prog.s

# 3. assemble to an RV32 object
build/skj-as-rv -o prog.o prog.s

# 4. link the crt, the program, and the freestanding runtime into a static ELF
build/skj-ld-rv -o prog.elf \
    build/kpfree-rv/kp_crt_rv.o \
    prog.o \
    build/libkpaot-rv-free.a

# 5. load and run in the RV32 sandbox
build/skj-run prog.elf
```

`skj-run` is a drop-in for `qemu-riscv32` on this path, so
`qemu-riscv32 prog.elf` runs the same ELF as an independent cross-check.

## The link order matters

The three link inputs go in this order, and the order is load-bearing:

1. `build/kpfree-rv/kp_crt_rv.o` is the crt: `_start`, the syscall wrappers over
   `ecall`, the bump arena, and the coroutine context switch. It must come first
   so `_start` is the entry.
2. `prog.o` is your compiled program.
3. `build/libkpaot-rv-free.a` is the freestanding guest runtime: the compute
   runtime (`str_core`, `dynarr`, `map`), the guest heap over a handed-in region,
   the console and exit seam, and the 64-bit integer and coroutine helpers. It
   comes last so the linker pulls only the archive members the program
   references.

This is the freestanding tier (`KP_FREESTANDING`, double-precision reals). It is
distinct from the hosted `libkpaot-rv.a` that `kobold/aot-run.sh` links against a
libc for a Linux or qemu run.

## Running it as a spawned child instead of the root program

The steps above run the program as the root ELF the emulator loads. The RV32
sandbox can also spawn a compiled guest as a child process (its own CPU, memory,
heap, and stack) over `RV_SYS_SPAWN`. `tests/xlang-sandbox/` is the worked
example: a C root process (built with `skj-cc-rv`) spawns a Kobold Pascal child
built through this same `skj-kpc-rv` to `skj-as-rv` to `skj-ld-rv` chain, proving
a C guest and a Kobold Pascal guest are the same kind of thing to the sandbox,
both being RV32 ELFs from the one shared IR. See `make test-rv-xlang-sandbox` and
`doc/tools/emulator.md`.

## The compiler itself in the sandbox

Stage 1 above runs the front end natively on the host, but that is a choice, not
a requirement. `kpcom` is an ordinary Kobold Pascal program, so it compiles
through this same RV32 and ColdFire pipeline into a guest ELF and runs inside
`skj-run`. The in-guest compiler is a verified fixed point: the KPM1 it emits is
byte-identical to the KPM1 the host compiler emits for the same source.

Two tiers of check cover it, on both the RV32 and the ColdFire guest:

- `make test-rv-selfhost-kpc` and `make test-cf-selfhost-kpc` compile a small
  program (`hello.pas`) with the in-guest `skj-kpc` and compare the emitted KPM1
  to the host compiler's. Fast, suitable for routine runs.
- `make test-rv-selfhost-kpc-full` and `make test-cf-selfhost-kpc-full` compile
  `kpcom`'s own source (`kobold/kpcom.pas`, about 480 KB) with the in-guest
  compiler and compare that KPM1 to the host's. This is the compiler reproducing
  itself in the sandbox. It is opt-in and slow, since the whole source runs
  through the RV32 or ColdFire interpreter, and it needs a larger guest heap
  (the target passes `--heap 256M`).

The guest ELF is built the same way a program is: `skj-kpc-rv` (or the ColdFire
`skj-kpc-cf`) compiles `kobold/kpcom.pas` straight to assembly,
`skj-as-rv` assembles, and `skj-ld-rv` links the freestanding crt, the compiler
object, and the freestanding runtime archive. Byte-identical output means the
in-guest compiler is a true peer of the host compiler, not an approximation:
anything the host compiler accepts and lowers, the guest compiler lowers the
same way, including the threaded-dispatch surface (`@label` computed goto and a
procedural value called through a computed designator). Compiling a program, or
compiling `kpcom` itself, runs in-guest with no host help.

## How the tests exercise this

`tests/run-kp-src-tests.sh` runs exactly this pipeline in its `free` mode:
`skj-kpc-rv` emits the module, `skj-kpc-rv` compiles it to assembly, `skj-as-rv`
assembles, `skj-ld-rv` links the crt, program, and freestanding archive, and the
ELF runs under `skj-run` (passed as the `QEMU` prefix) or `qemu-riscv32`. The
`make check-kp-io-free` and `check-kp-inc-free` targets drive it, so the path is
covered in CI, not only documented here.
