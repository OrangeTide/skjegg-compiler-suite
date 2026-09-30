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

The ELF loader has an in-memory form beside the path form. `elf32_load`
now slurps the file and delegates to `elf32_load_mem`, which walks the
program headers over a caller-owned buffer and does no file I/O;
`elf32_symbol` and `elf32_load_raw` are the same split. This is what lets
a host load a guest from an image it already holds in memory (a vendored
verb, a downloaded module) rather than only from a path. Every emulator
suite exercises the `_mem` body through the path wrappers.

## The RV32 interpreter loop

`rv_run(cpu, budget, retired)` runs up to `budget` instructions and
returns **why** it stopped, an `enum rv_run_reason`:

| Reason | Meaning |
|---|---|
| `RV_RUN_BUDGET` | ran the full count; still runnable |
| `RV_RUN_HALT`   | halted: a clean exit or a double fault |
| `RV_RUN_TRAP`   | a trap was taken; `cpu->mcause` names the cause |
| `RV_RUN_YIELD`  | the hart is parked (`cpu->waiting`): `wfi`, still runnable |

`*retired`, when non-NULL, receives the count. The loop stops the moment
the machine halts, a trap is taken, or the hart parks on `wfi`, rather
than spinning the rest of the batch through a vector with no handler
installed. Halt outranks a trap (a double fault sets both, and the host
wants the halt); a parked hart reports as a yield, not a stop, because it
is still runnable once the host clears `cpu->waiting` or an interrupt
does. This is the cooperative-yield boundary a scheduler builds
coroutine-style tasks on, and it is the contract a JIT backend would
return through as well. `rv_step` is one instruction over the same loop,
for a host that drives the machine itself; it discards the reason and
reports only halt.

The dispatch is a single threaded loop over the major opcode, not a
call per instruction. The exceptional exits (misaligned PC, fetch and
bus faults, illegal encodings, the interrupt path, the per-instruction
stop test) are marked `RV_UNLIKELY` so the compiler keeps the cold trap
trampolines out of the straight-line fetch and dispatch path. That is a
code-layout aid, not a prediction fix: profiling shows every one of
those branches is already predicted near-perfectly, and the measurable
win (about 7% on a mixed-workload guest, best-of-runs) comes entirely
from tightening the hot path's instruction footprint. The two branches
that actually mispredict, the compressed-versus-32-bit discriminator
(near 50/50) and the dispatch switch itself (an indirect branch), are
inherent and cannot be hinted.

A **decoded-instruction cache** (`rv_dec_enable`) speeds re-execution: a
direct-mapped table keyed by guest PC holding the expanded 32-bit form
and length, so a re-executed PC skips the fetch-and-expand. It is a
hint, so a miss only costs the work it was meant to save. `skj-run`
enables it on every process; a host that does not want it simply leaves
`cpu->dec` NULL and the slow decode runs every instruction, which is
what the coverage build does (see `RV_ICOV`). `fence.i` invalidates the
whole cache: without that a guest that rewrites an instruction and then
runs `fence.i` (self-modifying code, a guest-side JIT) would keep
running the stale decode. Because it is a hint it must change no result,
and `make test-rv-decode-cache` pins exactly that: it runs a program
with the cache on and off in lockstep and compares the architectural
state after every instruction. One of its programs rewrites an
instruction it already ran, runs `fence.i`, and runs it again, so a
missed invalidation shows as a divergence between the two cores.

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
would anything notice. The fuzzer adds almost no coverage and was for a
long time the only method to find a real bug in this core, because its
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

A sanitizer pass (`make check-emu-san`, ASan and UBSan over the
host-only tests) later found a defect none of the value methods could
see: the S/B/J immediate decoders sign-extended by shifting a negative
value left, which is undefined behavior. It produced the right bits on
two's-complement hardware, so every value method, the fuzzer and
lockstep included, ran straight over it, and the coverage audit reported
those lines as reached the whole time. Reach and sensitivity both ask
whether the answer is right; the sanitizer asks whether the code that
produced it is defined, an axis the value methods have no view of.

## Verification gaps and future work

The peripheral layers added since the core, the guest allocator
(`emu/guest.c`) and the process scheduler (`emu/rv_user.c`), do not have
the core's verification depth, and a run of reviews found several bugs in
them: `fence.i` not flushing the decode cache, a 32-bit wrap in
`gm_mmap`/`gm_munmap`, `do_spawn` leaving the parent's `mem.fault` set,
and `proc_slice` never clearing `cpu->waiting`. Each of those now has a
regression test, and `make check-emu-san` rebuilds the host-only tests
with ASan and UBSan (it found an undefined left-shift in the S/B/J
immediate decoders on its first run). Three follow-on items would close
the rest of the gap. They follow the same lesson the audit above teaches:
sensitivity, not reach.

1. **A differential for every internal optimization, seeded with its
   adversarial case.** The decode cache is transparent to a guest, so
   qemu is not its oracle; cache-on versus cache-off in one process is
   (`make test-rv-decode-cache`). The `fence.i` bug was the invalidation
   trigger, not the steady state, so the rule is that a cache or hint's
   differential must exercise every invalidation trigger, not just
   re-execution of the same code. A targeted fuzz mode that emits
   store-to-text plus `fence.i` sequences and compares cache-on against
   cache-off would be cheaper and more sensitive for that class than a
   qemu lockstep, which cannot see the cache at all.

2. **A boundary-value table for guest address and size arithmetic.**
   `gm_mmap` and `gm_munmap` compute near the 32-bit boundary, which is
   where the wrap bug hid. Every size or address entry point should be
   driven with a standing table of values around the edges (0, 1, one
   below a page, a page, one page below 4GB, 4GB, and 4GB plus a guard),
   rather than the specific overflow cases now sitting in
   `tests/test_guest_mmap.c` that were added once the bug was known.

3. **Scheduler state invariants and a stress guest.** Both `rv_user.c`
   bugs were shared state left set across a lifecycle boundary. A
   debug-build invariant checked between quanta (a runnable process has
   `mem.fault == 0`, and `cpu->waiting` matches the reason its last
   `rv_run` returned) would turn that class into an immediate abort
   instead of a misbehavior found by reading. The one sandbox demo is a
   happy path; a stress guest that spawns many children, exhausts the
   per-child and aggregate memory budgets, and mixes `wfi` park and wake
   would drive the transitions those two bugs lived in.

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

## Guest mmap and guarded stacks (RV32)

A guest can ask the emulator for a fresh, guarded region at run time.
`gm_mmap(m, size, flags)` maps `size` bytes (page-rounded) at an
emulator-chosen address in a dynamic area above the image, heap and
stack, returns the base, and with `GM_MAP_GUARD_LO` places an unmapped
guard page just below it. A guard page is simply a region with
protection 0, so any access to it faults with the reason "stack
overflow" and sets `fault_guard`. `gm_munmap(m, base, size)` releases
the region and its guard. There is also an aggregate commit budget
(`struct gm_pool`, shared across guests), though nothing wires it up
yet.

`gm_mmap` computes the whole extent (an optional guard page, the
mapping, and a trailing gap) in 64-bit and refuses anything that would
leave the 32-bit space, so a near-4GB request cannot wrap the cursor and
hand back a low address that overlaps the image, heap, or an earlier
mapping. It reserves the region-table slots up front, so a full table
never leaves a guard page mapped with no mapping behind it. `gm_munmap`
likewise ignores a range whose base plus size wraps past 4GB rather than
walking a bad page count. These bounds are guest self-corruption
concerns, not host escapes: a guest address is never a host address, it
is translated through the per-process page tables to a host page.

The guest reaches this through an **emulator-private ecall**, not a
Linux syscall: `a7 = 0x000f0001` (`RV_SYS_MAP`, `a0` size, `a1` flags,
returns base in `a0`), `a7 = 0x000f0002` (`RV_SYS_MUNMAP`), and `a7 =
0x000f0003` (`RV_SYS_SPAWN`, below). Those numbers sit far above the
Linux RISC-V syscall space, so a real kernel or `qemu-user` answers
`-ENOSYS` and a guest that wanted a guarded region falls back to
whatever it does without one.

That fallback is exactly how the Excelsior source coroutines work on
both runners from one binary. `libexc` calls an optional
`__exc_src_stack_provider` hook for a coroutine's private stack;
`start_rv_psabi.S` installs `__exc_map_stack`, which issues the
`RV_SYS_MAP` ecall with the guard flag. On `skj-run` that returns a
guarded region, so a coroutine stack overrun faults on the guard page
instead of silently corrupting the arena. On `qemu-user` the ecall
returns `-ENOSYS`, the provider returns 0, and `libexc` falls back to an
arena block. `make check-exc-rv-emu` runs the coroutine tier through the
guarded path on `skj-run`; the full `qemu-riscv32` suites cover the
arena fallback.

Two host-side tests pin these primitives directly, needing no cross
toolchain and no qemu. `make test-guest-mmap` drives `emu/guest.c` with
no CPU: it checks the guard page faults on an overrun, `gm_munmap`
releases the region and its guard, the `gm_pool` aggregate budget caps a
commit, `gm_read_out` copies a range out, and the overflow bounds hold
(a near-4GB mmap is refused with no dangling guard, a wrapping munmap is
a no-op). `make test-rv-run` pins the
`rv_run` batch contract (the reason it returns and the early stop on a
trap, a park, or a halt).

## The RV32 process sandbox

`skj-run` runs one root process, and a guest may spawn children. Each
process has its own CPU, guest memory, and heap and stack, so a child
shares no memory with its parent. A round-robin scheduler runs the live
processes a `QUANTUM` of instructions at a time; the batched `rv_run`
returns after each slice with the reason it stopped, so the scheduler
reaps a process that exited, halted, or faulted, and reschedules one that
merely spent its slice. This is the consumer the `rv_run` reason contract
was built for.

`RV_SYS_SPAWN` (a7 `0x000f0003`, `a0` an image pointer, `a1` its length,
returns a handle) loads a child from an image the guest holds in its own
memory: the host reads the bytes out with `gm_read_out` and loads them
with the in-memory ELF loader into a fresh process. A returned handle is
a slot plus a generation, so a handle held past a child's death does not
match a later child that reuses the slot.

The `--total-mem` cap is an aggregate `gm_pool` budget shared across every
process, so many children cannot together exhaust host memory even though
each keeps its own per-process `--mem` limit. `--child-max-insns` budgets
a spawned child so a runaway child is killed alone rather than stalling
the run.

`make test-rv-sandbox` is the end-to-end demo. A parent guest, built by
the in-tree skj toolchain (`skj-cc-rv` + `skj-as-rv` + `skj-ld-rv`), spawns
a child embedded in its own data and does a guarded map and release; the
child maps a large region and touches every page. Under a generous
`--total-mem` both finish; under a tight one the child exhausts the shared
pool and faults while the parent, isolated from it, still finishes and the
run exits on the root's status. That one demo exercises spawn, the guarded
map and release, the shared budget, and the scheduler resuming a child
across many slices.

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

One of the seven was not really a float program: `exs_source_yield` has
no doubles and used to fail because `start_rv_psabi.S` spills fs0-fs11
with `fsd` on every source switch, and the F-only core cannot execute
`fsd`. A coroutine switch has to preserve those registers, and the
backend allocates them as doubles, so the honest fixes were a build
variant or nothing. The build variant now exists: `make check-exc-rv-emu`
runs the coroutine tier on `skj-run` with a soft-ctx crt, a mechanical
`fsd`->`fsw` / `fld`->`flw` rewrite of `start_rv_psabi.S` (the Makefile
generates it with `sed`). That is correct on this core precisely because
it runs no double, so the saved float slots only ever hold 32-bit values.
See "Guest mmap and guarded stacks" below for the private stack those
coroutines run on.

## Single-precision determinism across backends

The single-precision core is not just a convenience, it is the
deterministic floating-point surface the VM hosts build on. A game that
runs its simulation on a smolmoo server and predicts it on a client
needs both sides to compute the same `float` bits, or the prediction
drifts from the authority. The core is built for exactly this: it
depends on no host rounding mode (see above), so its `fadd.s` and the
rest are a fixed function of their inputs on any host, and that function
is verified bit-for-bit against qemu by the fuzz, lockstep and
riscv-arch-test rigs.

That guarantee holds for two peers **running the same core**. It does
not automatically extend to two peers on different code generators. The
plain `test-f32-*` suites do not catch the difference: they use only
exact integer-valued operands, which every IEEE-ish FPU agrees on. The
interesting cases are the inexact, subnormal, NaN and conversion results
where hardware FPUs are free to differ.

`make test-f32-diff` measures that. The oracle is the RV32 core itself:
`tests/f32_oracle.c` drives `emu/rv32.c` over a vector table
(`tests/f32_vectors.h`) and freezes the results as a committed golden
master (`tests/f32_golden.inc`). Each backend's `skj-cc` then compiles
`tests/f32_diff.c`, runs it under that backend's qemu, and checks every
vector against the golden, naming the first divergence.

The finding is that **only peers all running the RV32 emulator are a
verified f32 sync set.** The native-FPU backends diverge from the core
on IEEE-underspecified behavior, and they do not even agree with each
other:

- x86-64 and MIPS produce the opposite NaN sign for `0 / 0`
  (`0xffc00000` where the core canonicalizes to `0x7fc00000`).
- AArch64 agrees there but differs on signaling-NaN quieting.
- ColdFire cannot hold a bit-exact binary32 at all: its FPU computes in
  a wider internal format, so injecting an exact single already loses
  the pattern.

These are reported as XFAIL, not build failures, because they are facts
about the hardware rather than regressions. The one hard gate is
`test-f32-diff-rv`, which catches a real codegen regression in the
shared f32 core (that would break many vectors, not one NaN sign), and
`f32-golden-check` enforces that the committed golden tracks the vector
table. Making a native backend a valid sync peer would mean
canonicalizing NaNs in its codegen, which is not done and is not needed
as long as the peers share the emulator.

This is also what makes an f32 Excelsior practical. Excelsior's `float`
is a double today, off the deterministic surface (see above). The IR,
every backend, and this test already carry a proven single-precision
path, so moving Excelsior's `float` to `IR_F32` would land it inside the
determinism the games need, with no new emulator work.

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
