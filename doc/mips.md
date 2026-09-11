# The MIPS I backend

A code generator for MIPS I, the instruction set of the MIPS R2000 and
R3000. The two chips share one ISA, so a single binary runs on both. The
R3000 added pipeline interlocks that the R2000 lacks, so this backend
generates code that is correct on the stricter R2000, which the R3000 then
runs unchanged.

The target is little-endian (`mipsel`), the byte order DEC chose for the
DECstation and Sony chose for the PlayStation. The DECstation 2100 and 3100
pair the CPU with an R2010 or R3010 floating-point unit. The PlayStation's
R3051 has no FPU at all. That split is why floating point is a configuration
choice here rather than a fixed feature, discussed under "Floating point"
below.

Status: milestone 5 complete, milestone 6 (a stand-alone toolchain and a
PlayStation target) done: the assembler `skj-as-mips`, the linker `skj-ld-mips`
(with an `ar` reader and a PS-EXE output mode), the `skj-ar` archive tool, and
the BIOS-TTY runtime are in place, so the MIPS toolchain is binutils-free
(`as`, `ld`, `ar`) and writes the PlayStation executable format. The PS-EXE runs
on the PCSX-Redux emulator booting a real BIOS, and the soft-float configuration
runs float code there on the FPU-less R3051. The backend is complete
for two tiers. The stack
convention (the toolkit's own) runs TinC, TinScheme, and the C compiler; the
o32 platform ABI runs Excelsior against a gcc-built C runtime. It passes the
TinC and TinScheme suites (`make check-mips`), the `test-i64`, `test-ops`,
`test-fpu`, and `test-f32` integration tests, the C compiler suite
(`make check-cc-mips`, 73 tests), and the Excelsior suite
(`make check-exc-mips`, 174 tests), all under `qemu-mipsel`. A `.set
noreorder` mode (`SKJ_MIPS_NOREORDER=1`) makes the backend fill the delay
slots itself with a scheduler; every suite has a `-noreorder` variant that
passes. A basic inline-assembly statement (`asm` / `asm volatile`) passes
hand-written text through untouched. A soft-float configuration
(`-DMIPS_SOFTFLOAT`) targets the FPU-less PlayStation R3051, lowering every
float operation to an integer softfloat library; `make check-mips-sf` passes
the float suites through it. See "Roadmap" for what remains.

## Build and test

```sh
make check-mips        # TinC + TinScheme, compiled, assembled, linked, run
make test-i64-mips     # 64-bit integers (IR builder -> asm -> qemu-mipsel)
make test-ops-mips     # unsigned 32-bit ops
make test-fpu-mips     # double float, conversions, compares, half round-trip
make test-f32-mips     # single precision
make test-softfloat    # the soft-float library, host-only, against native double
make check-mips-sf     # the whole soft-float tier (library + fpu/f32 + C suite)
```

`check-mips` runs the same 16 programs as `make check-rv`, through the same
harness (`tests/run-tests.sh`). The four `test-*-mips` targets are the same
IR-builder integration tests the other backends run.

Running one test by hand:

```sh
./build/skj-tinc-mips -o build/mips/hello.s tests/hello.tc
mipsel-none-elf-as -march=mips1 -EL -o build/mips/hello.o build/mips/hello.s
mipsel-none-elf-as -march=mips1 -EL -o build/mips/start.o runtime/start_mips.S
mipsel-none-elf-ld -o build/mips/hello build/mips/start.o build/mips/hello.o
qemu-mipsel build/mips/hello ; echo $?
```

## The toolchain

The backend emits GAS assembly and hands it to an external assembler and
linker, the same arrangement the RISC-V, x86, and AArch64 backends use. The
toolchain is `mipsel-none-elf`, a bare-metal (newlib) cross toolchain
whose defaults are exactly the R2000/R3000 profile:

```
-march=mips1   -mabi=32 (o32)   -EL   -mfp32   -mno-abicalls
```

MIPS I, the o32 ABI, little-endian, 32-bit floating-point registers, and
non-PIC code. The backend uses only the assembler and linker from this
toolchain, not its C library. The runtime is the hand-written
`runtime/start_mips.S`, which issues Linux system calls that `qemu-mipsel`
services. This is the same split the RISC-V backend uses: a
`riscv64-linux-gnu` toolchain assembles and links, but the test runtime is
`start_rv.S`, not glibc.

`qemu-mipsel` is the user-mode emulator, the counterpart of `qemu-riscv32`.
It loads a static ELF32 MIPSEL executable and emulates the Linux o32 system
call convention. `start_mips.S` follows that convention: the system call
number goes in `$v0` (the o32 number space starts at 4000, so `write` is
4004 and `exit` is 4001), arguments in `$a0` through `$a3`, and the `syscall`
instruction traps into the emulator.

## The hazards, and how they are handled

MIPS I exposes three pipeline hazards that later MIPS revisions hide behind
interlocks, plus a fourth question about the FPU. A code generator that
targets the R2000 must answer all four.

### The load delay slot

On the R2000, the instruction immediately after a load cannot use the loaded
register. The load has not landed yet, so the reader sees stale data. The
R3000 stalls the pipeline instead, so the same code works there, which is
exactly the trap: code that runs on an R3000 can carry a hidden bug that only
shows on an R2000.

### The branch delay slot

The instruction after any branch or jump always executes before the transfer
takes effect. This is true of both chips. A branch or jump in that slot is
undefined.

### Multiply and divide latency

`mult` and `div` write their results to the `hi` and `lo` registers
asynchronously, taking several cycles. Reading `lo` with `mflo` too soon
returns stale data on the R2000.

### How milestone 1 handles all three

By emitting `.set reorder` assembly and letting the assembler fill the slots.
`.set reorder` is the assembler's default. For `-march=mips1` it fills the
load delay slot, fills the branch delay slot, and inserts the `mult`-to-`mflo`
latency, either by scheduling a safe independent instruction into the slot or
by inserting a `nop`. It also expands the macro instructions the backend
leans on: `li`, `la`, `move`, `mul`, `div`, `rem`, `beqz`, `bnez`, and the
rest.

This is correct on the R2000, and it matches the toolkit's standing choice to
lean on the assembler rather than carry a scheduler. There are no optimization
passes anywhere in this compiler, by design, and this backend adds none.

The cost is real and worth stating plainly. Leaning only on `.set reorder`
cannot express the code where a programmer must control the delay slots by
hand: boot code, interrupt service routines, exception handlers, and
`volatile` inline assembly. Those need the backend to emit `.set noreorder`
and fill the slots itself, and a path that hands a hand-written block through
untouched. Milestone 1 did neither and generated ordinary application code
only. Both are now in place: see "The noreorder scheduler" and "Inline
assembly" below.

### Floating point

MIPS I floating point lives on coprocessor 1. The DECstation has that
coprocessor as a physical R2010 or R3010. The PlayStation does not have one
at all, so a native FPU instruction there is an illegal-instruction trap.

The default build targets the hardware FPU, which `qemu-mipsel` models and
which the DECstation has. A soft-float configuration for the PlayStation,
where every float operation becomes an integer helper call, is built with
`-DMIPS_SOFTFLOAT` on the same backend and is described under "Soft-float
(PlayStation)" below.

Three MIPS I specifics shape the floating-point lowering, because the R2010
and R3010 are MIPS I FPUs and lack instructions that later revisions added:

- There is no `trunc.w`, the convert-to-integer-with-truncation instruction
  (MIPS II added it). A C float-to-int cast truncates toward zero, so `IR_FTOI`
  reads the FP control register with `cfc1`, sets its rounding mode to
  round-toward-zero, runs `cvt.w`, and restores the register.
- There is no `ldc1` or `sdc1`, the 64-bit doubleword loads and stores for
  coprocessor 1 (also MIPS II). A double is loaded and stored with the
  assembler macros `l.d` and `s.d`, which expand to two `lwc1` or `swc1`
  word transfers.
- There is no hardware half-float. `IR_FLH` and `IR_FSH` call the integer-only
  softfloat helpers in `runtime/half.c` (`__skj_extendhfsf` and
  `__skj_truncsfhf`), the same helpers the x86 and ColdFire backends use.
  Those helpers are `gcc`-built o32 functions, so unlike every other call in
  this backend, these two use the o32 register convention: the argument in
  `$a0`, the result in `$v0`. They touch no floating-point or callee-saved
  register, so the mid-selection call is safe.

Comparisons follow from another MIPS I trait: there is no compare-to-register
instruction. `c.eq.d`, `c.lt.d`, and `c.le.d` set a condition flag in the FP
control register, and the backend materializes the boolean by branching on it
with `bc1f`. A NaN operand makes those comparisons false, which matches the
"zero on NaN" behavior of the other backends.

A double occupies an even/odd register pair named by the even register, and
o32 has six callee-saved double registers, `$f20` through `$f30`. The
allocator uses those six. A single-precision value lives in the low register
of a pair.

## The register model

The o32 register file, and how this backend uses it:

| Register        | Use                                                   |
|-----------------|-------------------------------------------------------|
| `$zero` ($0)    | always zero                                           |
| `$at` ($1)      | assembler temporary, left alone (macros clobber it)   |
| `$v0` ($2)      | return value, and scratch for a call result           |
| `$v1` ($3)      | free scratch                                          |
| `$a0`-`$a3`     | free scratch (our calls pass arguments on the stack)  |
| `$t0`, `$t1`    | reload scratch for spilled operands                   |
| `$t2`-`$t8`     | free scratch                                          |
| `$t9` ($25)     | indirect call and tail-call target                    |
| `$s0`-`$s7`     | allocatable, callee-saved (8 integer registers)       |
| `$k0`, `$k1`    | reserved for a kernel, unused                          |
| `$gp` ($28)     | unused (non-PIC code needs no global pointer)         |
| `$sp` ($29)     | stack pointer                                          |
| `$fp` ($30)     | frame pointer                                          |
| `$ra` ($31)     | return address                                         |

The register allocator (`backend/regalloc_mips.c`) is the shared
Poletto-Sarkar linear scan, the same code every backend runs, with the
integer class set to the eight `$s` registers. `$at` is never allocated,
because the assembler's macro expansion of `li`, `la`, and the branch
pseudo-instructions clobbers it.

## The calling convention

The default convention is the toolkit's own stack-passing one, shared with
the ColdFire and the non-psABI RISC-V backends. It is not the o32 C ABI. That
means this backend's functions call each other, and call the runtime in
`start_mips.S`, but they do not interoperate with a `gcc`-compiled o32 object.
TinC, TinScheme, and the C compiler use this convention. The `CC_PSABI` build
(next section) emits the o32 ABI instead, for the Excelsior tier.

The rules:

- Arguments are pushed on the stack, four bytes each, and the caller pops
  them after the call.
- The return value comes back in `$v0`.
- `$s0` through `$s7`, `$fp`, and `$ra` are callee-saved.

Frame layout after the prologue, with `$fp` as the anchor:

```
$fp + 8 + 4*i    argument i (the caller pushed it before the jal)
$fp + 4          saved $ra
$fp + 0          saved old $fp
$fp - locals     bottom of the locals
below locals     spill slots
$sp + 0..28      saved $s0..$s7
```

The prologue sets `$fp` to the entry stack pointer minus 8, so argument `i`
sits at `$fp + 8 + 4*i`, which is where the caller wrote it. The whole frame
is rounded up to a multiple of 16 so `$sp` stays 16-aligned across calls.

## The o32 platform ABI (skj-exc-mips)

`skj-exc-mips` is built `-DCC_PSABI` and emits the o32 C ABI, so a gcc-built
guest runtime (libexc and its host binding) links against it and is called in
both directions. This is the counterpart of the RISC-V `CC_PSABI` build. The
runtime objects are cross-compiled with `mipsel-none-elf-gcc`, and the
entry point, syscalls, arena, and source coroutines come from
`start_mips_psabi.S`, the o32 version of `start_mips.S`.

The o32 argument rules used here:

- Arguments fill a conceptual argument structure one word at a time. The
  first four words go in `$a0`-`$a3`; the rest go on the stack. The caller
  always reserves at least the 16-byte register home area, which is where a
  callee spills its register parameters.
- A two-word argument (a `long long`, or a double) is 8-aligned in that
  structure, so it starts on an even word. With that alignment it never
  splits across the register/stack boundary.
- A leading run of floating-point arguments (at most two, before any integer
  argument) is also passed in `$f12` and `$f14`, and a double result comes
  back in `$f0`. This is the hard-float o32 rule, and it is what a gcc-built
  `__exc_str_from_float(double)` reads.
- Other results come back in `$v0`, or `$v0:$v1` for two words.

The parameter side reuses `slot_offset` unchanged: it already lays parameters
out at `$fp+8` upward, 8-aligned for 8-byte slots, which is exactly the
argument structure. The prologue spills the register parameters (`$a0`-`$a3`)
into those homes, and the rest of the lowering reads every parameter from its
slot as before.

Two decisions make the float path work with this specific toolchain, which
ships a single hard-float libgcc multilib:

- The runtime is compiled hard-float (the default). A soft-float runtime
  would pass doubles in integer registers, which the backend could match, but
  its double math would call the soft-float libgcc helpers (`__adddf3`, ...),
  and the toolchain has no soft-float libgcc. Hard-float keeps libexc's double
  math on the hardware FPU and its few libgcc calls (`__fixdfsi`, since MIPS I
  has no `trunc.w`) in the matching hard-float ABI.
- A leading floating-point argument is passed in `$f12`/`$f14` *and* in the
  integer home slot. A gcc callee reads `$f12`; a skj callee reads the home
  slot (it has no per-parameter float classification of its own in the
  Excelsior front end). Writing both is one spare move and keeps the two
  callee kinds correct without the caller having to know which it is calling.
- gcc is invoked with `-G 0` so it addresses globals absolutely rather than
  through `$gp`, which the hand-written `_start` does not set up.

## Instruction selection notes

Most opcodes map straight across from the RISC-V emitter, since both are
flagless load-store machines. A few points where MIPS differs and the
backend has to be careful:

- Arithmetic uses the non-trapping forms. `IR_ADD`, `IR_SUB`, and `IR_NEG`
  become `addu`, `subu`, and `negu`. The plain `add`, `sub`, and `neg` trap
  on signed overflow, which is not the wrapping semantics C wants. Address
  and frame arithmetic uses `addiu` for the same reason.
- `andi` zero-extends its 16-bit immediate, so it cannot mask with a negative
  constant. The `alloca` alignment and the arena allocator's round-up load
  `-4` into a register with `li` and then `and`, rather than `andi $r, $r, -4`.
- The register-variable shifts are `sllv`, `srav`, and `srlv`. Their operand
  order matches the IR: destination, value, shift amount.
- MIPS has no compare-and-set-equal instruction. `IR_CMPEQ` is `xor` then
  `sltiu $d, $d, 1` (one if the xor was zero), and `IR_CMPNE` is `xor` then
  `sltu $d, $zero, $d` (one if the xor was nonzero). The ordered comparisons
  use `slt` and `sltu` directly, with an `xori $d, $d, 1` to invert for the
  `<=` and `>=` forms, exactly as the RISC-V backend does.
- Multiply, divide, and modulo use the assembler's three-operand macros
  `mul`, `div`, `divu`, `rem`, and `remu`. These expand to `mult` or `div`
  followed by `mflo` or `mfhi`, with the `hi`/`lo` latency handled by
  `.set reorder`. The signed `div` and `rem` macros also emit the
  divide-by-zero and overflow checks.

## Continuations

TinC and TinScheme both lower `call/cc`-style capture to three IR opcodes,
`IR_MARK`, `IR_CAPTURE`, and `IR_RESUME`, backed by two runtime routines. The
backend and `start_mips.S` implement the same stack-copying protocol the
RISC-V runtime uses:

- `IR_MARK` records the frame pointer, stack pointer, and a re-entry label
  into a three-word slot, and points `__cont_mark_sp` at it. The capture
  point first arrives with `$v0` set to zero.
- `IR_CAPTURE` pushes the eight callee-saved registers and calls
  `__cont_capture`, which bump-allocates a buffer, copies the live stack
  segment into it, and longjmps back to the mark's re-entry label with the
  buffer address in `$v0`.
- `IR_RESUME` calls `__cont_resume`, which copies the saved stack segment
  back and returns into it with the resume value in `$v0`.

The return register is `$v0` throughout, where the RISC-V version uses `a0`.

## The noreorder scheduler

By default the backend emits `.set reorder` and the assembler fills the delay
slots (see "The hazards"). With `SKJ_MIPS_NOREORDER=1` the backend emits
`.set noreorder` and fills the slots itself, which is what systems code (boot,
interrupt and exception handlers) and `volatile` inline assembly need, since
they cannot let the assembler move instructions around.

The scheduler runs over each function's finished assembly, captured through a
memory stream, in two passes:

- Branch/jump delay slot. Every `j`, `jal`, `jalr`, `jr`, `beqz`, `bnez`, and
  `bne` gets its one delay slot filled. The scheduler hoists the preceding
  instruction into the slot when that is provably safe: the instruction is a
  single-instruction ALU op or store, it is not itself another delay slot,
  and the branch does not read the register the instruction writes (which is
  what keeps `move $t9, target` out of a `jalr $t9` slot). Otherwise it
  inserts a `nop`. `bc1f` and `bc1t` always take a `nop`, because the
  instruction before them is the FP compare's settle gap and must stay put.
- Load delay slot. After a load, or a coprocessor move (`mtc1`, `mfc1`,
  `cfc1`), the scheduler inserts a `nop` when the very next instruction reads
  the loaded register. MIPS I has a one-instruction load delay with no
  interlock.

The `mul`, `div`, and `rem` macros stay the assembler's job: it fills their
internal `mult`/`div`-to-`mflo`/`mfhi` latency and their divide-by-zero check
even under `.set noreorder`, because each is one logical operation it
expands. The runtime `.S` files stay `.set reorder`; only the code the
backend generates is scheduled.

Two limits are worth stating. First, the load pass is deliberately narrow: it
covers the documented one-slot hazards (integer and FP loads, and the
coprocessor moves), not FP arithmetic result latency, which the R2010/R3010
interlock. Second, this correctness is by construction from the MIPS I hazard
rules. It is not observable under `qemu-mipsel`, which models the interlocked
pipeline and so runs unfilled slots correctly; it matters on real R2000
silicon. What the `-noreorder` test variants do verify is that the
scheduler's rewrites (the hoists in particular) preserve behavior: a wrong
hoist changes a result and fails under qemu regardless of interlocks.

## Inline assembly

`skj-cc` has a basic inline-assembly statement: `asm("...")` and `asm
volatile("...")` (with the `__asm__` and `__asm` spellings) emit their string
into the generated assembly verbatim. Adjacent string literals concatenate,
and the usual escapes (`\n`, `\t`) let one statement carry several lines. This
is basic asm only: no operand constraints, no `%0` substitution, and no
file-scope asm. The `volatile` qualifier is accepted and, since basic asm is
already an ordered barrier, has no further effect.

The statement lowers to one `IR_ASM` instruction carrying the text, which
every backend emits as-is; it is not the MIPS backend's feature alone. What is
specific to MIPS is its place in the `noreorder` scheduler. Under `.set
noreorder` the programmer owns the delay slots inside the block, so the
scheduler must not fill or reorder around them. The emitter brackets the text
with `#skj-asm-begin` / `#skj-asm-end` sentinels, and `sched_emit` copies that
region through untouched while scheduling the generated code on either side as
separate segments. Scheduling each segment alone also stops a hoist from
crossing an asm boundary, since a branch at a segment start sees no preceding
instruction to pull into its slot. Under `.set reorder` the text is emitted
plainly and the assembler treats it like any other code.

This is what a `noreorder` ISR body or a boot sequence needs: hand-written
MIPS that the toolchain passes through exactly as written.

## Soft-float (PlayStation)

The PlayStation's R3051 has no coprocessor 1, so a native FPU instruction
there traps. Building the backend with `-DMIPS_SOFTFLOAT` (the `skj-cc-mips-sf`
tool) emits no coprocessor-1 instruction at all: every float value is an
integer bit pattern and every float operation is a call into
`runtime/softfloat.c`, an integer-only IEEE-754 binary64 implementation.

The representation folds into the machinery already there. A `double` is a
64-bit value carried exactly like an `IR_I64`, in a register pair or an
8-byte slot; a `float` is a 32-bit value carried like an `int`. The register
allocator reads each float definition's width and puts a double in the i64
class and a single in the integer class, so a float value flows through the
same storage, spill, argument, and return paths as an integer of its width.
There is no float register class, so a soft-float function saves no `$f`
register and a build for it references none.

Each float opcode lowers to a helper call made with the o32 register
convention (arguments in `$a0`.., a 16-byte argument home, results in
`$v0`/`$v0:$v1`), the same way the half-float helpers are already called. The
helpers are `gcc`-built and preserve `$s0`-`$s7`, so a call is safe in the
middle of instruction selection: the allocated temps, which live in the `$s`
file or in slots, are untouched. `add`, `sub`, `mul`, `div`, the compares,
and the int and single conversions are calls; sign-only `neg` and `abs` are
inline integer `xor`/`and` on the sign bit, no call. A float argument keeps
the same 8-byte, 8-aligned footprint the callee's parameter layout expects,
so the hardware and soft-float callers are interchangeable at the ABI: a
double fills both words, a single the low word.

Single precision is computed by widening. A binary32 operation extends its
operands to binary64, runs the double core, and rounds the result back to
binary32. Binary64 carries more than twice a single's significand, so this is
exact for the four arithmetic operations and the compares. This is the same
decision the RISC-V single-precision path documents.

`runtime/softfloat.c` never reads or sets a host rounding mode, so it stays
WebAssembly-clean like the rest of the toolkit. It leans only on the target
toolchain's 64-bit integer helpers (`__udivdi3` and the like), which are
integer routines unrelated to the FPU and always present.

Verification is in three layers. `make test-softfloat` checks the library on
the host against the host's own hardware `double` as an oracle, across
arithmetic, compares, conversions, subnormals, and specials, with no cross
toolchain or emulator (the `test-gc` pattern). `make test-fpu-mips-sf` and
`test-f32-mips-sf` run the IR-level float suites through the soft-float
backend under `qemu-mipsel`, and each first asserts that no `$f` register
appears in the generated assembly, which is what proves the output would run
on a machine with no FPU. `make check-mips-sf` adds the whole C suite compiled
soft-float. `qemu-mipsel` models an FPU and so cannot tell soft-float from
hard-float on its own; the `$f`-register assertion is the check that does.

## Files

```
backend/regalloc_mips.c    linear scan, o32 register counts, soft-float classes
backend/mips_emit.c        instruction selection, GAS output, noreorder scheduler,
                           soft-float lowering
runtime/start_mips.S       stack convention: _start, syscalls, arena, i64,
                           continuations
runtime/start_mips_psabi.S o32 psABI: syscalls, arena, source coroutines
runtime/softfloat.c        integer-only IEEE-754 binary64 (soft-float builds)
tests/test_softfloat.c     host-only unit test of softfloat.c against native double
```

The Makefile builds `skj-tinc-mips`, `skj-sc-mips`, `skj-cc-mips`,
`skj-exc-mips`, and `skj-cc-mips-sf` (the soft-float C compiler) from these
plus the shared IR, and the `check-*-mips` targets drive the suites through
`mipsel-none-elf-{as,ld}` and `qemu-mipsel`. The FPU and C tests also link
`build/mips/half.o`, the `mipsel-none-elf-gcc` build of `runtime/half.c`,
for the `IR_FLH`/`IR_FSH` softfloat helpers; the soft-float builds add
`build/mips/softfloat.o`.

## The stand-alone toolchain and PlayStation target (milestone 6, planned)

Milestone 6 gives the soft-float configuration a way to run on real R3051
hardware, not just under `qemu-mipsel` with the no-coprocessor-1 assertion.
The vehicle is a MIPS toolchain that does not depend on GNU binutils
`as`/`ld`/`ar`, built over the shared assembler skeleton (`as/asm_lex`,
`as/asm_obj`) and the linker's layout core, the way the RISC-V
`skj-as-rv` / `skj-ld-rv` toolchain is. This is the "real demand" the
assembler-scope policy names: producing a PlayStation executable means owning
the linker, so the whole MIPS toolchain moves in-tree.

The decided approach:

- **`skj-as-mips` replicates `.set reorder`.** It expands the macros the
  backend emits (`li`, `la`, `mul`, `div`, `rem`, the branch pseudos) and fills
  the branch and load delay slots, so the current backend output assembles
  unchanged. The syntax is a chosen subset, roughly compatible with GNU as and
  the original MIPS assembler, which are both a reference. This is the opposite
  choice from the `SKJ_MIPS_NOREORDER` scheduler, which fills the slots in the
  backend; the assembler tier keeps that work in the assembler where the other
  toolchains put it.
- **ELF first, then PS-EXE.** `skj-as-mips`, `skj-ld-mips`, and archive
  support (an `ar` reader plus a `skj-ar`) emit ELF32 MIPSEL objects and
  executables first. A `check-*-mips-skj` tier then runs the existing suites
  under `qemu-mipsel` through the skj toolchain, with the assembler's encoding
  checked byte for byte against GNU as, the way `check-cc-rv-skj` does for
  RISC-V. Only after that does `skj-ld-mips` gain a second output mode that
  writes the PS-EXE executable format the console loads.
- **The PlayStation runtime is BIOS TTY.** The PS-EXE counterpart of
  `start_mips.S`'s Linux `write`/`exit` routes output through the BIOS A0/B0
  call tables, which is what most homebrew uses and is portable across the
  console models. No GPU model.

Verification is three complementary jobs, because no single available
simulator both models the R2000 pipeline exactly and loads a PS-EXE:

1. **`check-*-mips-skj` under `qemu-mipsel`.** The skj toolchain assembles and
   links the existing suites: functional parity with GNU as, and the branch
   delay slot, which qemu models.
2. **A `spim` tier over generated assembly.** `spim` models the R2000 load
   delay accurately, the hazard the noreorder discussion notes `qemu-mipsel`
   cannot see. Running the generated code through it exercises that hazard on
   both the `.set reorder` assembler output and the noreorder scheduler output.
3. **The PS-EXE path through a retargetable BIOS shim.** The BIOS TTY calls are
   one indirection, so the PlayStation program self-checks under `spim` (or
   `qemu`) with the BIOS call backed by the simulator's own output syscall,
   while the PS-EXE header and layout are validated structurally. There is no
   full PlayStation emulator in the automated gate; MARS (a Java GUI simulator)
   stays a manual reference.

Jobs 2 and 3 converge: running the PlayStation program under `spim` with the
BIOS TTY shimmed to the `spim` print syscall both exercises the program end to
end and catches a load-delay bug in one run.

### First sub-step: skj-as-mips, ELF first (done)

The first sub-step is the assembler alone; the linker and the PS-EXE output
come after. It mirrors the RISC-V port, plugging a front end (`mips.h`,
`mips_parse.c`, `mips_encode.c`, `mips_elf.c`, `mips_sched.c`, `mips_main.c`)
into the shared skeleton, with a `skj-as-mips` Makefile target beside
`skj-as-rv`.

The assembler accepts exactly what `backend/mips_emit.c` emits and nothing
more: the integer R-, I-, and J-type instructions, the COP1 instructions the
`fpu`/`f32` suites need (not the PlayStation soft-float path, which emits no
COP1), the macros `li`, `la`, `move`, `negu`, `beqz`, `bnez`, `b`, `mul`,
`div`, `divu`, `rem`, `remu`, `l.d`, `s.d`, and the directives `.text`,
`.data`, `.globl`, `.word`, `.short`, `.byte`, `.ascii`, `.space`, `.align`,
and `.set`.

Three pieces are new relative to `skj-as-rv`: the `.set reorder` engine
(delay-slot filling, the noreorder scheduler's hazard rules ported to the
encoded instruction stream), macro expansion, and the MIPS relocations
(`R_MIPS_32`, `R_MIPS_26`, `R_MIPS_HI16`, `R_MIPS_LO16`, `R_MIPS_PC16`), whose
HI16/LO16 carry-pairing is the analog of the `PCREL_HI20`/`LO12` pairing the
RISC-V linker already carries.

The byte-exact decision landed with one refinement discovered in the build.
`skj-as-mips` reproduces GNU as **byte for byte** for instruction encoding and
macro expansion, including the **full signed `div`/`rem` trap sequences** (the
divide-by-zero `break` and the `INT_MIN / -1` overflow check). It does **not**
match GNU as's `.set reorder` slot filling byte for byte: GNU's reorder
heuristic and the backend's own noreorder scheduler differ in both directions
(each inserts a `nop` the other omits, and GNU hoists into `jal` slots more
aggressively), so a byte match there would be a from-scratch reproduction of
GNU's version-specific algorithm. The `.set reorder` engine instead fills the
slots correctly (it ports the backend's proven scheduler) and is verified by
running, not by diffing. So byte-exactness holds where it is cheap and
checkable, the `.set noreorder` output, and correctness is the bar for
`.set reorder`.

The build order, all done:

1. Skeleton fit and driver (`mips.h`, `mips_main.c`, the `asm_lex` `$`-register
   knob).
2. The core encoder (`mips_encode.c`): the integer R-, I-, and J-type
   instructions and COP1, byte-identical to GNU as.
3. The ELF writer (`mips_elf.c`): ELF32 MIPSEL objects, the REL relocation set,
   and the local-symbol folding onto section symbols that a `la` or `.word` to
   a local label needs.
4. Macro expansion (`mips_encode.c`): `move`/`li`/`la`/`negu`/`neg`/`not`/the
   branch pseudos/`l.d`/`s.d`, `mul`, and `div`/`rem` with the full trap
   sequences, byte-identical to GNU as.
5. The `.set reorder` engine (`mips_sched.c`): branch and load delay-slot
   filling as a source pre-pass, `.set noreorder` regions passed through.
6. The verification harness (`tests/mipsas/`, `make check-mipsas`): the
   encoding golden-master against `mipsel-none-elf-as`, runnable programs under
   `qemu-mipsel`, and the `spim -delayed_loads` R2000 tier (`skj-as-mips -S`
   emits the scheduled assembly for `spim` to run on its accurate load-delay
   model). `make check-cc-mips-skj` adds the whole C suite assembled by
   `skj-as-mips` on its default `.set reorder` output.

Verified: the encoding and macro fixtures and all real backend `.set noreorder`
output are byte-identical to GNU as; the full 73-test C suite and the
tinc/scheme programs assemble through `skj-as-mips` on `.set reorder` output,
link, and run under `qemu-mipsel`; and the `spim` tier confirms the scheduled
code is correct on the R2000 model the interlocked `qemu` cannot show.

### Second sub-step: skj-ld-mips, ELF (done)

The linker mirrors `skj-ld-rv`: a front end (`mips_ld.h`, `mips_elf_read.c`,
`mips_link.c`, `mips_elf_write.c`, `mips_main.c`) over the arch-neutral layout
core (`ld_layout` / `ld_find_output_sec` / `ld_check_undefined` from `link.c`),
the script parser (`script.c`), and the mapfile reader (`mapfile.c`). It reads
ELF32 little-endian `EM_MIPS` relocatable objects and writes an `ET_EXEC` based
at `0x00400000` for `qemu-mipsel` user mode, with a `skj-ld-mips` Makefile
target beside `skj-ld-rv`.

Two things differ from the RISC-V linker:

- **REL, not RELA.** MIPS relocations carry the addend in the relocated field,
  not in the relocation entry, so each addend is read from the field before
  patching. The reader stores a zero addend and the writer's `HI16` handler
  reads the paired `LO16` field to recover the combined addend. Applied:
  `R_MIPS_32` (`S + A`), `R_MIPS_26` (absolute 26-bit word address, region
  checked), `R_MIPS_PC16` (a branch displacement), and the `R_MIPS_HI16` /
  `R_MIPS_LO16` pair. The pair combines its addends the way the o32 ABI
  specifies, `AHL = (HI << 16) + sign_extend(LO)`, resolving both fields against
  `S + AHL` with the `+ 0x8000` carry into the high half. This is the analog of
  the RISC-V `PCREL_HI20` / `LO12` pairing, and a `HI16` is resolved together
  with the next `LO16` in the object.
- **A `.MIPS.abiflags` record.** `qemu-mipsel` reads the `PT_MIPS_ABIFLAGS`
  program header to choose the floating-point register mode. An o32 MIPS-I
  image wants `fp_abi = DOUBLE` and FR=0; with no abiflags header `qemu` picks
  the wrong mode and every double-precision computation produces garbage (this
  was found the hard way: stripping the section from a GNU-linked binary breaks
  it the same way). The writer emits the 24-byte `Mips_elf_abiflags_v0` record
  byte-identical to GNU's, described by a `PT_MIPS_ABIFLAGS` program header and
  a section header. The default script declares two program headers so
  `ld_layout` reserves the header space, keeping the file-offset to vaddr
  congruence the single-segment writer relies on.

Verified: the full 73-test C suite assembled by `skj-as-mips` and linked by
`skj-ld-mips` runs under `qemu-mipsel` (`make check-cc-mips-skj`), and the
`run` tier of `make check-mipsas` links its hand-written assembly with
`skj-ld-mips` too.

### Third sub-step: archive support (done)

`skj-ld-mips` reads `ar` archives (`mips_archive.c`), the RISC-V archive reader
with `mips_parse_elf` used for a pulled member (the `ar` container is
arch-neutral, so the rest is identical). An input with the `!<arch>\n` magic is
consulted through its GNU symbol index rather than linked whole, pulling only
the members that resolve a still-undefined symbol and iterating to a fixpoint
(a pulled member can reference further members).

`skj-ar` (`ar/main.c`) is the archive creator that closes the loop. It writes a
GNU archive with a symbol index (the `s` of `ar rcs`, which the linker needs to
find members), lists members (`t`), and extracts them (`x`). It is generic over
the object machine and byte order: it reads each member's ELF32 symbol table
(either endianness) for the defined global symbols the index records, and
copies member bytes verbatim, so one `skj-ar` serves every skj target. Long
member names go in the GNU `//` string table, and the output is deterministic
(zero timestamps, mode 0644). This is target-neutral tooling, not MIPS-specific,
but it lands here because the MIPS toolchain is the one being taken
binutils-free end to end.

Verified: `make test-mips-archive` compiles three members, packs them with
`skj-ar`, and links a program that references one (which transitively pulls a
second) with `skj-ld-mips`, leaving the third out; a wrongly pulled unused
member would fail the link because it calls an undefined symbol, so a clean run
to exit 42 under `qemu-mipsel` proves both the reader's selection and the
tool's index. The archive format is cross-checked against GNU `nm`/`ar` on
little- and big-endian objects, and GNU `ld` links `skj-ar` output.

The MIPS toolchain is now binutils-free (`as`, `ld`, and `ar`).

### Fourth sub-step: PS-EXE output (done)

`skj-ld-mips -f ps-exe` writes the PlayStation executable the console loads
(`mips_psexe_write.c`), the second output mode beside the ELF one. A PS-EXE is
a 2048-byte header followed by a flat text+data image the BIOS loader copies
into RAM, so the writer does not touch the ELF program/section machinery: it
takes the sections `ld_layout` placed and `mips_ld_link` relocated and
serializes the loaded image plus the header the loader reads. The header names
the entry (`pc0`), the load address and size (`t_addr` / `t_size`, a multiple of
2048), the bss to clear (`b_addr` / `b_size`), and the stack base (`s_addr`).
A PS-EXE build uses the built-in PlayStation script (`mips_default_psx_script`,
base `0x80010000` in main RAM) unless a `-T` script overrides it; no `.set`
abiflags or program headers are involved.

The runtime is the BIOS-TTY crt `runtime/start_psx.S`, the counterpart of
`start_mips.S`. Its `_start`, `write`, and `exit` are the three entry points
that touch the outside world; `_start` sets the stack, clears bss, and calls
`main`, `write` sends each byte through the BIOS `A0(3Ch)` `std_out_putchar`
call, and `exit` writes `main`'s return value to the PCSX-Redux software-exit
register (`0x1f802082`) and then halts (the PlayStation has no process to return
to; the store is a no-op on real hardware and quits the emulator in test mode).
Everything below those, the 64-bit integer helpers, the arena, and the
continuation capture/resume, is pure computation identical to `start_mips.S`, so
the same compiled programs link.

Verification has two tiers. The first is structural (`make test-mips-psexe`,
always on): it links a few C programs to PS-EXE with the BIOS-TTY crt and
`tests/run-psx-tests.sh` validates the container, the `PS-X EXE` magic, the load
address and stack base, `t_size` a whole number of 2048-byte sectors matching
the file, and the entry inside the image. It also disassembles the payload raw
and confirms the jumps target the PlayStation base `0x8001xxxx` and never the
Linux base `0x0040xxxx`, so the writer used the PS-EXE layout and relocated at
the console address.

The second tier actually runs the executable (`make test-mips-psexe-redux`,
opt-in). It boots a real BIOS ROM on the PCSX-Redux emulator and loads each
PS-EXE. The BIOS-TTY crt's exit path writes `main`'s return value to the
PCSX-Redux software-exit register (`pcsx_exit`, a halfword store to
`0x1f802082`, harmless on real hardware), so in `-testmode` the emulator quits
with that code and the run self-checks like the qemu tiers. The tier has two
phases: integer programs run natively on the R3051 (`skj-cc-mips`), and a float
program, which would trap on the FPU-less R3051, runs through the soft-float
compiler (`skj-cc-mips-sf`, no coprocessor-1) linked with `softfloat.o`. That
second phase is the milestone-6 goal reached on an emulated console: float code
running on the FPU-less R3051. It is opt-in because it needs PCSX-Redux and a
BIOS ROM (`make test-mips-psexe-redux PSX_BIOS=/path/to/scphXXXX.bin`), and is
skipped cleanly when either is absent. MARS stays a manual reference.

The MIPS toolchain is binutils-free (`as`, `ld`, `ar`), reaches the PlayStation,
and its output runs on an emulated console booting a real BIOS; the soft-float
configuration runs float code there without an FPU.

## What the backend does not cover yet

- Large frames. The prologue and epilogue use `addiu`, whose immediate is a
  signed 16-bit value, so a frame larger than about 32 KB would fail to
  assemble. `gcc` materializes such offsets through `$at`; this backend does
  not yet. No test reaches that size.
- Extended inline assembly. Basic `asm` / `asm volatile` is in place (see
  "Inline assembly"), but there are no operand constraints or `%0`
  substitution, so hand-written assembly cannot yet name a C variable. Basic
  asm covers a `noreorder` ISR body written against fixed registers.
- A PlayStation target to run soft-float on real hardware. The soft-float
  configuration is in place and verified (see "Soft-float (PlayStation)"), but
  there is no PS1 executable format, BIOS, or GPU model in-tree, so it is
  exercised under `qemu-mipsel` with an assertion that no coprocessor-1
  instruction is emitted, not on a PlayStation. This is milestone 6, and its
  vehicle is a stand-alone binutils-free MIPS toolchain (see "The stand-alone
  toolchain and PlayStation target").
- Pascal and MooScript. Neither front end was ported to a non-ColdFire
  backend (RISC-V did not bring them up either), so they are not part of the
  MIPS tier. The C compiler and Excelsior are.

## Roadmap

1. Integer core, control flow, calls, `alloca`, continuations. `check-mips`
   passes TinC and TinScheme. (Done.)
2. Floating point on the hardware coprocessor (o32 double pairs, the
   `l.d`/`s.d` macros since MIPS I has no `ldc1`/`sdc1`, FCSR-based truncation
   since it has no `trunc.w`, and `half.c` for `_Float16`) and 64-bit
   integers (register pairs plus the ported `__muldi3` and shift helpers).
   Passes the `test-i64`, `test-ops`, `test-fpu`, and `test-f32` integration
   tests. (Done.)
3. The C compiler (`check-cc-mips`, stack convention) and the o32 psABI
   configuration for Excelsior (`check-exc-mips`), so a gcc-built C runtime
   links. The counterpart of `check-cc-rv` and `check-exc-rv`. (Done.)
4. The `.set noreorder` scheduler, so the backend fills the delay slots itself
   for systems code (`SKJ_MIPS_NOREORDER=1`, verified by the `-noreorder` test
   variants), and a basic inline-assembly channel in `skj-cc` (`asm` / `asm
   volatile`, passed through the scheduler as a barrier). (Done.)
5. PlayStation soft-float (`-DMIPS_SOFTFLOAT`): every float operation lowers to
   a call into the integer-only `runtime/softfloat.c`, so no coprocessor-1
   instruction is emitted and the code runs on the FPU-less R3051. Verified by
   `make check-mips-sf` (the softfloat library on the host, the fpu/f32 suites
   with a no-`$f`-register assertion, and the C suite compiled soft-float).
   (Done.) Remaining on this track: extended asm with operand constraints.
6. A PlayStation target for the soft-float configuration, so it runs on real
   R3051 hardware rather than only under `qemu-mipsel` with the no-coprocessor-1
   assertion. The vehicle is a stand-alone MIPS toolchain that does not depend
   on GNU binutils `as`/`ld`/`ar`: `skj-as-mips` (replicating `.set reorder`),
   `skj-ld-mips`, and archive support, built over the shared assembler skeleton
   and the linker's layout core like the RISC-V toolchain, emitting ELF first
   and the PS-EXE format second, with a BIOS-TTY runtime. Verified by a
   `check-*-mips-skj` tier under `qemu-mipsel`, a `spim` tier for R2000
   load-delay accuracy, and a retargetable BIOS shim for the PS-EXE path. See
   "The stand-alone toolchain and PlayStation target" above for the full plan.
   (Done: `skj-as-mips`, `skj-ld-mips` with its `ar` reader and a `-f ps-exe`
   PS-EXE output mode, the `skj-ar` archive tool, and the BIOS-TTY runtime
   `start_psx.S` are in place, so the MIPS toolchain is binutils-free
   (`as`, `ld`, `ar`) and writes the PlayStation executable format, validated
   structurally by `make test-mips-psexe` and run on the PCSX-Redux emulator
   with a real BIOS by `make test-mips-psexe-redux` (integer programs native,
   float programs through soft-float on the FPU-less R3051).)
