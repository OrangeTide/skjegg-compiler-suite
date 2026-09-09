# skj-run: the in-tree guest runner

`skj-run` runs a compiled 32-bit guest program on an emulator built into
this tree, the way `qemu-user` or `wasmtime` runs one. It replaces
`qemu-m68k` and `qemu-riscv32` for the targets it covers, and it is the
only runner for strict ColdFire code, which qemu does not model.

```sh
skj-run hello.elf                  # architecture from the ELF header
skj-run --arch rv32 prog.elf a b   # override, and pass guest arguments
skj-run --trace --stats prog.elf   # CPU event ring on a fault, counters
```

The exit status is the guest's own. A guest killed by a fault reports
128 plus the signal a host kernel would have raised (139 for a bad
memory access, 132 for an illegal instruction), which is what a shell
reports for a real process, so a test harness needs no special case.

## Where it came from

The two CPU cores were written for the research articles and adopted
into skjegg, which owns them now. The RV32 series ran to five articles
and is finished, so upstream is a record of how the core was built
rather than a place it is maintained; the final version, its whole test
rig and the tooling that audited that rig were imported here (2026-08-05,
research commit 99a25a2). The articles keep a frozen copy of the code
they describe:

- <https://orangetide.github.io/the-mechanical-researcher/coldfire-emulator/>
- <https://orangetide.github.io/the-mechanical-researcher/coldfire-emulator-part-2/>
- <https://orangetide.github.io/the-mechanical-researcher/rv32-emulator/>
- <https://orangetide.github.io/the-mechanical-researcher/rv32-bitmanip/>
- <https://orangetide.github.io/the-mechanical-researcher/rv32-zcb/>
- <https://orangetide.github.io/the-mechanical-researcher/rv32-interrupts/>
- <https://orangetide.github.io/the-mechanical-researcher/rv32-coverage/>

Fixes land here, not there. Two have landed already: `REMS.L`/`REMU.L`
wrote the quotient into `Dq` as well as the remainder into `Dr`, which
is the 68020 pair form rather than the ColdFire split, and it masked a
matching `skj-as` bug (a two-operand `divs.l` was encoded with `Dr = 0`,
which real hardware reads as a remainder instruction).

## Layout

```
emu/coldfire.c   ColdFire V4e core (integer, FPU, EMAC)   \  one component
emu/cf_user.c    Linux/m68k user mode: vectors, syscalls  /

emu/rv32.c       RV32IMAFC + Zicsr, Zifencei, Zba/Zbb/Zbs, Zcb  \  the other
emu/rv_user.c    Linux/RISC-V user mode: ecall            /

emu/guest.c      sparse memory, the syscall layer, the initial stack
emu/elf32.c      ELF32 loader, either endianness
emu/main.c       the driver
```

The two cores are independent components. A project vendoring skjegg
takes `emu-cf`, `emu-rv`, or both; `EMU_COLDFIRE=0` / `EMU_RV32=0` drop
the other core out of the build entirely. This matters because the
target hosts differ: smolmoo is ColdFire, and a host that moved to RV32
would carry only the RISC-V core.

## The machine

Guest memory is a sparse two-level page table over the 32-bit address
space, committed 4 KB at a time on first touch inside a mapped region.
The loader maps one region per `PT_LOAD` segment with that segment's
protection, then the heap (for `brk`) and the stack are mapped above
the image. An access outside every region is a fault reported with the
address and the reason, rather than a silent read of zero.

Syscalls are the handful a freestanding guest reaches: `read`, `write`,
`writev`, `brk`, `close`, `exit`, `exit_group`. The per-architecture
number spaces (Linux/m68k and the RISC-V generic table) map onto one
arch-neutral set, so a call added once reaches both targets. Anything
else answers `-ENOSYS`, the way Linux would, so an unexpected call
surfaces as a guest-visible error.

ColdFire has no syscall hook in the core, and it does not need one:
user mode is built out of the hardware behavior. The vector table lives
in a page of guest memory with every entry pointing at a distinct
address in an unmapped magic window, so when the PC lands there the
driver knows which vector fired. `TRAP #0` is serviced and returns
through the exception frame; anything else is reported as the signal a
kernel would have raised. RV32 uses the core's `ecall` callback.

## The two test tiers

`make check` cross-compiles the C runtime for generic m68k, which is
what `qemu-m68k` models. gcc then emits 68020 instructions (the 64-bit
divide, byte and word arithmetic on memory, `bfextu`) that neither a
real ColdFire nor this emulator has.

`make check-emu` rebuilds that runtime with `-mcpu=5475` and relinks
into `build/cf`. The compiled test objects are unchanged, since every
front end already emits ColdFire. That build is what a boris or smolmoo
host would load, so the emulator tier is the closer model of the real
target; qemu stays as an independent oracle for the generic-m68k build.
Note that qemu cannot run the ColdFire tier at all, `-cpu cfv4e`
included: it rejects `mov3q` and the remainder instructions.

| Target             | Runner    | Runtime built for |
|--------------------|-----------|-------------------|
| `make check`       | qemu-m68k | generic m68k      |
| `make check-emu`   | skj-run   | ColdFire (5475)   |
| `make check-rv`    | qemu-riscv32 | RV32IM         |
| `make check-rv-emu`| skj-run   | RV32IM (same binaries) |
| `make check-exc`   | qemu-m68k | generic m68k      |
| `make check-exc-emu` | skj-run | ColdFire (5475)   |

`make check-emu-all` runs every suite the emulator covers, with no qemu
involved.

## Excelsior on RV32, and the platform ABI

`make check-exc-rv` runs the whole Excelsior suite on RISC-V: 174 of
174 under `qemu-riscv32`, the same count ColdFire passes.

Getting there was a calling-convention problem, not a missing-feature
one. The RV backend passes every call argument on the stack, which is
the toolkit's own convention; the RISC-V ILP32 ABI passes the first
eight in `a0`-`a7`. On m68k the two coincide, which is why gcc-built
`libexc.c` and `skj-exc` output call each other for free there. On RV32
they could not, in either direction: compiled code reaches about 40
`__exc_*` entry points, and `__exc_list_map` and `__exc_send` call back
into compiled code, so a shim layer would have needed trampolines both
ways.

So the RV backend gained a platform-ABI path under `CC_PSABI`, the flag
x86-64 and AArch64 already use, and `skj-exc-rv` is built with it. The
rules it implements, all confirmed against what gcc emits:

- one-word arguments take `a0` through `a7` in order;
- a two-word argument takes the next two registers, low word first, and
  the pair is **not** aligned to an even register the way ARM's EABI
  aligns it;
- with one register left, a two-word argument splits: low word in the
  register, high word first in the outgoing block;
- the rest go in the block, two-word arguments 8-aligned;
- a double crosses through memory (RV32 cannot move the halves of an
  f-register to integer registers), a single through `fmv.x.w`;
- results come back in `a0`, or `a0` and `a1`.

A register parameter is spilled to a home slot in the prologue, so the
rest of the lowering keeps reading parameters from slots and does not
know the difference. A tail call whose arguments all fit in registers
stays a real tail call; one with a stack argument becomes a call and a
return, since the outgoing block would sit under the frame being torn
down. Only the Scheme front end depends on the tail property, and it
does not target this ABI.

`runtime/start_rv_psabi.S` is the matching runtime: entry, syscalls,
arena, and the source coroutines under that convention.
`runtime/soft64.c` supplies the 64-bit helpers, since the cross
toolchain ships no rv32 libgcc. Both retire a trap that was already in
the tree: `start_rv.S` defines `__muldi3` and the shift helpers with the
stack convention under the standard names, so gcc-built C calling one
would read its arguments from the wrong place. It had not fired only
because gcc inlines the 64-bit multiply libexc needs.

## The RV32 test rig, and what it is worth

The RV32 core came out of a five-article series that is now finished,
so skjegg is where it is maintained. What came with it matters as much
as the core: six verification methods, each runnable alone, in
`tests/rv32`.

| Method | What it is | Target |
|---|---|---|
| `unit` | seven suites, 443 checks, no reference model | `make test-rv32-unit` |
| `program` | one compiled guest that validates its own results | `make test-rv32-program` |
| `apps` | CoreMark and Lua, which validate their own results | `make test-rv32-apps` |
| `archtest` | 268 compliance tests against qemu-system-riscv32 | `make check-archtest` |
| `fuzz` | randomized encodings against qemu-riscv32, per instruction | `make test-rv32-fuzz` |
| `lockstep` | compiled guests against qemu-riscv32, per instruction | `make test-rv32-lockstep` |

`make check-rv32` runs the two that need no reference model and no
download. Measured on import: 443 unit checks pass, the self-checking
guest passes, arch-test is 268 passed / 0 failed / 4 skipped, lockstep
runs 21,708 instructions across four guests with no divergence, and the
fuzzer compares 25,600 instructions with none.

The `apps` method is neither vendored nor built by default, since each
needs a checkout and Lua needs a second toolchain:

```sh
make test-rv32-apps CM=<path>/coremark LUA=<path>/lua
#   git clone --depth 1 https://github.com/eembc/coremark.git
#   git clone --depth 1 -b v5.4.7 https://github.com/lua/lua.git
#   apt-get install gcc-riscv64-unknown-elf picolibc-riscv64-unknown-elf
```

What those buy over the hand-written guests is that nobody wrote them
with this emulator in mind. CoreMark validates its own workload CRCs
(30.4M instructions retired here). Lua is the harder of the two: it
allocates constantly, uses `setjmp`/`longjmp` for errors, formats
floating point, and runs a script that checks its own answers, so if it
passes then most of a C library and most of the instruction set are
working together (15.0M instructions, all checks pass). Lua needs a C
library the stock riscv cross toolchain does not have, which is what
picolibc is for.

Being able to run each method alone is what let the series measure what
each is worth, which is the part worth keeping:

```
make audit-rv32-coverage   # which lines each method reaches
make audit-rv32-icov       # which guest instructions each method runs
make audit-rv32-mutants    # whether a wrong answer would be noticed
```

Instruction coverage exists because line coverage is the wrong unit for
an emulator: one switch arm serves eight instructions, so a method can
run every line of the decoder while touching a fraction of the
architecture. Reproduced in this tree:

```
method         seen    of 171    unique cumulative
unit             98     57.3%         8         98
program          39     22.8%         0        129
apps             98     57.3%         0        156
archtest        155     90.6%         7        166
fuzz            101     59.1%         0        166
lockstep         89     52.0%         0        166

union           166     97.1%

never executed by any method:
c_ebreak csrrc csrrci csrrsi ebreak
```

The **unique** column is the one to read: instructions a method reaches
that no other method does, which is the closest thing coverage offers
to "what would be lost if this were deleted". Only two methods score
there at all.

That last list is the point. It is the same information as "185
uncovered lines" in a form a person can act on.

**The two measures disagree, and the disagreement is the lesson.**
Lockstep contributes no unique line and no unique instruction, so on
coverage it is redundant; mutation testing says it kills four mutants
nothing else kills. Coverage measures *reach*: did control flow arrive
here. Mutation measures *sensitivity*: if the answer here were wrong,
would anything notice. The fuzzer adds almost no coverage and is the
only method that has ever found a real bug in this core, because its
value is in the values flowing through a line rather than in reaching
it. Lockstep adds no coverage and supplies the only oracle written by
strangers; everything else compares against expectations written by the
same person who wrote the interpreter. Acting on either measure alone
would delete a method the project cannot do without.

The series also found a defect this way that had been present since the
first commit, and it is the one fixed in this tree: vectored `mtvec`
applied to exceptions as well as interrupts. It survived 268 compliance
tests, a hundred thousand fuzzed instructions and every lockstep run,
because no user-mode reference model has machine mode in it at all.
`gcov` had been reporting that line as uncovered the whole time, inside
a list nobody read, and the first article had excused it by name as
"defensive". It was not defensive; it had simply never been run.

## Conformance: riscv-arch-test

`make check-archtest ARCHTEST=<path>/riscv-test-suite` runs the RISC-V
compliance suite against the RV32 core. Current result:

```
riscv-arch-test: 268 passed, 0 failed, 4 skipped, 0 did not build
```

covering I, M, A, C, F, F_Zcf, B and Zifencei, in about three minutes.

The suite ships no reference signatures, so the harness produces them:
each test writes its signature over a serial port, and the same binary
is run on `qemu-system-riscv32` to get the same text, which is then
compared byte for byte. The dump is done by the guest rather than by
reading memory through a debugger, because attaching one changes what
is measured, qemu's gdb stub claims the guest's own `ebreak` instead of
letting it trap, and that trap is exactly what the `cebreak` test
exists to check.

`tests/archtest.c` is therefore a second, **bare-metal** machine beside
`skj-run`'s user mode: RAM at 0x80000000, a 16550 transmitter, a CLINT
and a test finisher, since compliance binaries are machine-mode images.
It shares the CPU core and the ELF loader (`elf32_load_raw`, the same
segment walk against a plain byte-write callback).

Two dependencies keep it opt-in rather than part of `check-emu-all`:
`qemu-system-riscv32` for the reference, and a checkout of
riscv-arch-test on branch **old-framework-3.x**. The suite is large and
external, so it is not vendored; `ARCHTEST` points at it, and the
target says what it needs rather than reporting a pass it did not run.
The main branch needs an assembler that pads `.p2align` with nops while
relaxation is disabled, and binutils 2.42 fills it with zeros, which
hangs those binaries on any model.

The 4 skips are honest gaps, not noise: three are Zbc carry-less
multiply, a separate extension from the B suite that this core does not
implement, and one is `C/cebreak-01`, whose signature holds `mstatus`, a
WARL register whose readable bits depend on which privilege modes exist:
qemu's virt CPU has machine, supervisor and user where this core has
machine only, so the two read back different legal values and the test
is not comparable between them.

## Interrupts (RV32)

The RV32 core takes machine-mode interrupts. A host raises and lowers
the three lines a single hart has:

```c
rv_set_irq(cpu, RV_IRQ_TIMER, 1);   /* raise */
rv_set_irq(cpu, RV_IRQ_TIMER, 0);   /* lower */
uint32_t cause = rv_irq_pending(cpu);   /* 0 when none would be taken */
```

`RV_IRQ_SOFT`, `RV_IRQ_TIMER` and `RV_IRQ_EXT` are the three, and they
are the bit positions in `mip` and `mie` as well as the low bits of the
`mcause` an interrupt produces. That is the whole seam: a timer or an
interrupt controller lives in the host, decides when a line is high,
and the core does the rest.

What the core does with it:

- An interrupt is taken **between instructions**, so `mepc` names the
  instruction that has not run yet and `mret` resumes at it. Nothing
  retires on that step.
- Both gates apply: `mstatus.MIE` globally, and the matching bit in
  `mie`. Enabling a bit later takes an interrupt that was already
  raised, because a line is a level and not an edge.
- Priority is the spec's: external, then software, then timer.
- The lines are **level-sensitive**, like the hardware they model. A
  handler that returns without lowering the line, or without clearing
  the bit in `mip` itself, is re-entered. That is a faithful model
  rather than a convenience, and a guest that forgets will livelock in
  its handler exactly as it would on real silicon.
- `wfi` is a nop, which the spec allows, and sets `cpu->waiting`. A
  host driving the machine step by step can read that and jump its own
  clock to whatever raises the next interrupt instead of stepping the
  wait out.
- Vectored `mtvec` (mode 1) sends interrupts to `base + 4*cause` and
  every exception to `base`. **This used to vector both**, so an
  exception landed on whatever entry its cause number happened to hit;
  the fix came with this work and `make test-rv-irq` pins it.

`make test-rv-irq` is a host-side test that drives the core directly
with hand-encoded guest programs, so it needs neither a cross toolchain
nor qemu. It covers delivery, both mask gates, priority, vectored mode
for interrupts and exceptions alike, `wfi`, and level re-entry.

## Double precision is a non-goal (decided 2026-08-04)

`check-exc-rv` runs under qemu, not `skj-run`, because the RV32 core
implements F but not D: `emu/rv32.c` rejects any instruction whose
format field is not single. Excelsior's `float` is an IEEE double, so
`fadd.d` and `fsd` are both unreachable. `skj-run` passes 80 of the 87
RV Excelsior binaries and fails 7 on exactly that.

**D will not be added.** The work is out of proportion to what it buys.
Six of those seven failures are programs that really do use doubles, so
the payoff is six tests; the cost is a software binary64 core.

The cost is not the instruction decoding, which is about 90 lines
mirroring the single-precision block, for the thirteen D mnemonics our
whole RV corpus actually uses. It is that this file states a property
worth keeping: nothing depends on the host FPU's rounding mode, so
`<fenv.h>` is never needed, because WebAssembly has no rounding-mode
control and a host mode makes results depend on whatever the embedding
process left behind. Single precision honors that cheaply by computing
in a host double and rounding down to single by hand. **For double
there is no wider host type** (`long double` is x87-specific and absent
on WebAssembly), so honoring it means a Berkeley-style softfloat
binary64: directed rounding, the inexact flag, and a 106-bit product
for the fused ops. That is a far larger and more delicate body of code
than the rest of the FPU put together, to run six tests that qemu
already runs.

So the RV tier is split by design: qemu-riscv32 is the runner for
anything with a double in it, `skj-run` for everything else. The same
gap covers `_Float16`, which the core also lacks (`fmt` must be S), so
a `_Float16` guest is qemu's too.

One of the seven is not really a float program: `exs_source_yield` has
no doubles and fails because `start_rv_psabi.S` spills fs0-fs11 with
`fsd` on every source switch. A coroutine switch has to preserve those
registers, and the backend allocates them as doubles, so the honest
fixes are a build variant or nothing. It is left as a known wart rather
than a reason to reopen D.

## Limits

- The RV32 core has F, and neither D nor Zfh, by decision rather than
  omission (see above). A guest using `float` or `_Float16` on that
  target belongs to qemu. ColdFire's FPU is full 64-bit, so the
  ColdFire tier has no such split.
- `skj-run` is user mode only. A bare-metal mode (memory-mapped
  devices, the shape the VM hosts use) is not exposed as a flag,
  though `tests/archtest.c` now shows the shape of one for RV32 and
  both cores support it.
- No debugger. The RV32 article has a GDB stub that has not been
  adopted yet.
- `skj-as` has no `rems`/`remu` mnemonic, so a front end cannot ask for
  the remainder instruction directly. The emulator implements it.
