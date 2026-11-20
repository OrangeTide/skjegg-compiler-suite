# An Optimization Adventure: What We Tried and What It Cost

A journal of a single exploration into post-0.57.0 optimization on the skjegg
backends. It records the paths taken, what each one actually bought, and what it
cost in code, complexity, and care. The point is the record, including the dead
ends, not a polished result.

## Where it started

Release 0.57.0 shipped move-coalescing on the two-address backends (x86-64,
ColdFire) on top of a shared control-flow-aware liveness pass. That raised the
question of what to optimize next, especially on the three-address backends
(RISC-V, MIPS, AArch64) where coalescing does not apply. The design note
`three-address-opt.md` had already measured the instruction mix and argued the
next gains come from cutting load and store traffic, not from the immediate or
move classes. This adventure tested that argument by building the passes and
measuring them, rather than trusting the prediction.

All measurements below are on the C compiler corpus (about 112 programs) compiled
to assembly, assembled, and counted at the post-assembly instruction level.
Everything was built behind an environment-variable gate, default off, so the
production path stayed byte-identical throughout.

## Path 1: a machine-code peephole for constant and address reuse

The first hypothesis was that constant and address reuse (keep a materialized
value in a register and reuse it) would combine well with a post-emit peephole
pass. The RISC-V backend already has a machine-code sink abstraction, so the
natural shape was a recording sink: the selector records tagged machine ops into
a buffer, a block-local peephole walks it, and the survivors flush to the text
sink. The known-value map resets at every label and call, which keeps it sound
without any dominance information.

What it found: the constant and address reuse that motivated the whole idea fired
**twice in the entire corpus**. After register allocation the selector almost
never leaves the same value live in a second register, so there is nothing to
reuse. Essentially all of the small win came from a different effect, redundant
re-materialization of the same value into the same register within a straight-line
run, which the pass simply deletes.

- Result: 172 instructions removed, about 0.69 percent.
- Breakdown: 163 redundant-rematerialization drops, 2 reuse-to-move rewrites, 0
  self-move drops.
- Lesson: real constant and address reuse has to happen at the IR level, before
  register allocation, so the allocator can keep one shared value in a register.
  At the machine-code level, after allocation, the opportunity has already
  evaporated. The synergy hypothesis was wrong in its specifics.
- Cost: moderate. It meant routing roughly seventy small emit functions through a
  record-or-print helper in one file, plus the peephole. The risk was contained
  because the gate-off path is byte-identical, which is cheap to verify against a
  pristine build.

## Path 2: load forwarding in the same recording sink

Since the recording sink already existed, the next step pointed it at the
dominant instruction class: loads and stores (about half of all instructions on
RISC-V). The peephole gained a slot map tracking which register holds which
memory cell, doing store-to-load forwarding and redundant-load elimination within
a straight-line run. Two details carried the soundness: an alias test that treats
different base registers as possibly-aliasing and the same base as aliasing only
on byte overlap, and a mnemonic check so that a value read by a sign-extending
load is never forwarded as if it were zero-extended, with store-to-load limited to
full words.

What it found: a real reduction in the class that matters.

- Result: integer loads fell from 7863 to 7659, about 2.59 percent, from 156 loads
  forwarded to register moves plus 48 redundant loads eliminated.
- The important nuance: a forwarded load becomes a register move, so it removes a
  memory access without removing an instruction. The honest metric here is memory
  accesses, not instruction count.
- Lesson: measure at the right granularity. An instruction count undersells a pass
  whose job is to cut memory traffic.
- Cost: moderate, an extension of the Path 1 machinery. The subtle part was the
  alias and extension reasoning, which is where a careless version would miscompile.

## Path 3: scalar register promotion, the cross-block prize

The straight-line peephole cannot cross a basic block boundary, and the design
note's biggest lever was register promotion: keep an address-not-taken local in a
register across its whole live range instead of round-tripping it through a frame
slot on every block boundary. This only became reachable once 0.57.0 gave the IR a
control-flow-aware liveness pass, because a promoted value lives across blocks.

The pass is small and the care is all in the soundness argument. A local slot
qualifies when it is a word scalar, is never address-taken, and is touched only by
the plain load and store of a local. Such a slot has no aliasing writes, because
every alias needs the slot-address op first. The slot becomes one temp: each store
turns into a move into it, each load into a move out of it. The single-interval
liveness keeps that temp in one place across all its defs and uses, so even stores
on separate paths into a join stay correct, last writer wins, exactly as memory
did. Parameter home slots are excluded, because every backend's prologue fills
them outside the IR, so a promoted temp would be undefined at its first use.

What it found on RISC-V:

- Memory accesses (loads plus stores) fell 2.69 percent from promotion alone, and
  4.07 percent when stacked with the Path 2 peephole.
- Instruction count stayed flat. On a three-address ISA the promoted store and load
  become register moves that have nothing to coalesce them away, so the memory
  access is gone but the instruction remains.
- Cost: about sixty lines of code, but the highest care-to-code ratio of the whole
  adventure. The effort was in the soundness reasoning (parameter homes, aliasing,
  the single-location equivalence), not the implementation.

## Path 4: generalize to a shared pass, and the payoff across the ISA divide

Promotion is backend-neutral, so it moved into a shared IR pass used by all five
backends. The interesting question was the two-address backends, because there a
promoted store's setup move is exactly what 0.57.0 coalescing elides. So on those
targets promotion should cut instruction count, not only memory traffic.

It does. The same pass behaves differently on the two kinds of ISA, and the split
is the whole story:

| backend | kind | instruction count | frame memory refs |
| --- | --- | --- | --- |
| RISC-V | three-address | flat | loads down 2.19 percent (promotion alone) |
| AArch64 | three-address | flat | down 3.68 percent |
| x86-64 | two-address | down 0.54 percent | down 8.75 percent |
| ColdFire | two-address | down 0.83 percent | down 11.0 percent |

On the three-address backends the win is purely fewer memory accesses, whether the
target is ILP32 (RISC-V) or LP64 (AArch64). On the two-address backends the
promoted moves coalesce into the surrounding operations, so the same pass also
reduces instruction count. The two AArch64 and x86-64 figures include the 8-byte
pointer and long slots, since both are LP64; the RISC-V and ColdFire figures are
word scalars only (the x86-64 and ColdFire rows here predate the 64-bit widening
of Path 5, so x86-64's numbers grow there). Promotion and the coalescing
shipped in 0.57.0 are complementary: coalescing had little to bite on by itself,
and promotion hands it exactly the moves it was built to remove.

- Cost: small. One new shared file, a one-line call at the top of each of the five
  allocators, and one line in the build. All five backends and their value-checking
  integration tests pass with the pass on.

## What the whole adventure cost

In code, the footprint is small: one recording sink and peephole in the RISC-V
text sink, and one shared promotion pass of about sixty lines wired into five
allocators. In complexity, the load-forwarding alias and extension rules and the
promotion soundness argument are the parts a reviewer must read carefully; the
rest is mechanical. In iteration, each path followed the same cheap loop: build
behind a gate, prove the gate-off path byte-identical, prove the gate-on path
passes the suites, then measure. That loop is what kept the risk flat while trying
several ideas.

Exact token or wall-clock figures for the exploration were not recorded, so this
journal does not claim them. The relative effort is clear enough: Path 1 and Path
2 were the most code for the least durable insight, and Path 3 was the least code
for the most.

## Path 5: widening promotion to pointers and longs on LP64

The first promotion cut handled only word scalars, which on the LP64 targets
leaves the pointers and longs, the 8-byte locals, still round-tripping the frame.
Widening to them needed one missing primitive: a 64-bit register-to-register
move. The IR had no such opcode, because a reg-reg copy of a 64-bit value had
never come up; reusing the 32-bit IR_MOV would have let the allocator spill the
value at four bytes and truncate a pointer. So the work was a new IR_MOV64, the
i64 twin of IR_MOV, appended last so opcode numbers stay put, classified as an
i64 def, and lowered in all four LP64 emitters (the two psABI selectors and the
two stack emitters that TinScheme uses). It also joins the x86 coalescing hint, so
on x86-64 the promoted setup move is elided rather than left behind.

Promotion then also takes an 8-byte slot touched only by the 64-bit local load and
store, under the LP64 build guard, with an access-class check that rules out a
slot mixing word and doubleword access.

What it found on x86-64: the pointer and long slots roughly matched the word
scalars in payoff. On top of word-only promotion they removed 110 more frame
memory references (down to 12.9 percent below baseline) and 70 more instructions
(0.90 percent below baseline). The lesson from Path 4 held: the two-address
backend converts the extra memory-traffic win into an instruction-count win,
because the wider promoted moves coalesce too.

Cost: small but spread wider than the earlier paths, because a new opcode touches
every place that lowers moves. The care was in one spot, the spill-width
classification, where getting it wrong would have truncated pointers under
register pressure rather than failed loudly.

## Path 6: sub-word locals, and a reasoning mistake worth recording

Widening promotion to char and short locals turned out to need a new IR
primitive, and the way it was first argued against is itself worth the note.

A sub-word local is stored by a word IR_STL but read through IR_ADL (or IR_ADL64
on LP64) feeding a sign or zero extending memory load, because the IR had no
register-to-register sub-word extension; the front ends only ever extend through a
load. So the address op taints the slot, and promoting it means replacing that
extending load with a register extension of the promoted word temp. No backend had
such an instruction exposed.

The first instinct was to not do it, on the grounds that adding the opcodes would
mean new machine-code primitives in three JIT byte-code encoders, framed as risk.
That argument did not hold up. Nothing was blocked and the JIT was not frozen; the
same session had already added IR_MOV64 to the JIT sinks. New encodings are normal
work, exercised by the test suites rather than silent, and a reg-to-reg sub-word
extend is a legitimate primitive the IR was simply missing. The effort was real,
but effort is not a constraint, and dressing it up as one was the error.

Done properly: four opcodes, IR_SEXTB / IR_ZEXTB / IR_SEXTH / IR_ZEXTH. Because
each is a single register op, the pass rewrite stays in place with no splicing: the
store becomes a move, the address op a nop, the load a register extension. The cost
was all in the lowering, across x86 (new movsx/movzx forms), AArch64 (native
sxtb/sxth/uxtb/uxth, which also had to be taught to the in-tree assembler, not only
the backend), RISC-V (shift pairs and a mask), MIPS (shift pairs and a mask), and
ColdFire (ext.w/ext.l and a mask). The assembler gap was caught by the test suite
running the output under the emulator, which is exactly the loud, non-silent
failure the risk argument had imagined was unavoidable.

The result is a clean win, not the instruction-count regression feared for the
shift-based alternative, because the dedicated extend replaces a two-instruction
lea-plus-load with one or two register ops. On x86-64 it adds a further 26
instructions and 32 frame references removed on top of the word and pointer
classes.

## Path 7: i64 on the ILP32 targets, where promotion can lose

The last unpromoted class was the 8-byte i64 local on the ILP32 targets. There a
64-bit value is a register pair, not a single register, so IR_MOV64 lowers to a
pair copy. The structured-pair model makes that safe without overlap analysis: by
lo/hi parity a destination pair is either identical to a source pair or fully
disjoint from it, a destination-lo can never land on a source-hi, so a plain
lo-then-hi copy with self-move guards is correct, exactly as the other i64 ops
already lower.

The measurement is the interesting part, because for the first time promotion is
not a free win. Isolating the i64 path on the cc corpus (frame memory refs):
RISC-V fell 64 and ColdFire fell 58, but MIPS rose 48. The regression is real and
has a clear cause: a promoted i64 holds a register pair for its whole live range,
and on MIPS, with fewer allocatable registers than RISC-V, that extra pressure
evicts other temps into spills that cost more memory traffic than the slot
round-trips it removed. The pass is correct on all three (the full C corpus passes
and the i64 integration tests pass), so this is purely a pressure trade-off, not a
soundness question.

- Lesson: promotion is not unconditionally profitable. It trades a frame slot for
  sustained register occupancy, and when the value is a pair on a register-poor
  target the trade can go negative. A profitability heuristic (promote only when
  pressure allows, or skip pair promotion on the tightest targets) is the obvious
  next step if this moves toward production.
- Cost: small, a pair-copy case in three emitters and dropping the LP64 guard on
  the 8-byte path. The value was the data point, not the code.

The gate itself is the first per-backend profitability knob. The pass takes a
promote_wide flag that selects whether the 8-byte slot shape is promoted at all;
the MIPS allocator passes 0 and every other backend passes 1, so MIPS keeps word
and sub-word promotion but skips the losing i64 pair. The flag lives at the call
site rather than behind a target macro because the MIPS tools do not share one
preprocessor define, while every MIPS build links the one MIPS allocator that
makes the call. With the gate in place MIPS returns to its word-and-sub-word frame
figure with no i64 regression, and RISC-V and ColdFire keep the pair win.

## The debug-info question, resolved

Every path above left one blocker standing: promotion moves a variable's home from
a frame slot into a register, so a `-g` build that tells a debugger the variable
lives in that slot would now be lying. The question was whether to suppress
promotion under `-g` or to track the moved location with DWARF location lists, the
second being a large amount of work.

Looking at what the toolchain actually emits dissolved the question rather than
answering it. The DWARF here is line-table debugging only: a `.debug_line` program
mapping an address to a file, line, and column, and a `.debug_info` holding one
`DW_TAG_compile_unit` shell with the name, low and high PC, language, and producer.
There is no `DW_TAG_subprogram`, no `DW_TAG_variable`, and no `DW_AT_location`
anywhere in the tree. The debugger can map a PC to a source line, set line
breakpoints, step, and symbolize a backtrace, but it has no notion of where any
local lives, because the compiler never says. A debug format that never describes a
variable's storage cannot be invalidated by moving that storage. Promotion is
therefore safe under `-g` with no DWARF work at all.

The line table itself is also untouched, for a separate reason. Source lines ride on
`IR_LOC` marker nodes (the line sits in the instruction's immediate, with no slot
and no temp operand), and the pass never removes, reorders, or even inspects them;
it rewrites slot loads and stores in place into register moves. So each surviving
instruction keeps its position in the marker stream and its line.

Both arguments were checked against the tools. With promotion forced on under `-g`,
the `-g` line and DWARF suites pass, including the one that makes the in-tree
`skj-addr2line` agree with GNU `addr2line` statement for statement and the one that
walks a trap backtrace. And on a function where promotion removes 43 percent of the
frame references, the emitted `.loc` directives are identical in count and in the
set of source lines they cover. The blocker is gone, not deferred.

One forward condition remains, bounded and not currently in the tree. If the
toolchain ever grows real variable-location DWARF (`DW_TAG_variable` with
`DW_AT_location`, `DW_OP_fbreg`, location lists, so a debugger can print a local),
then a promoted variable's location genuinely changes, from a frame slot to a
register and perhaps back under spill. At that point promotion owes such a variable
a location list, or must be suppressed for variables under `-g`. That is a cost for
whoever adds variable-location debugging, not a cost of promotion today.

## Lessons worth keeping

- Measure the thing the pass is for. A memory-traffic pass judged by instruction
  count looks like a failure when it is not.
- A reuse opportunity seen after register allocation is usually a mirage; the
  allocation already spent it. Constant reuse and value sharing belong at the IR
  level, before the allocator.
- The cheapest optimization infrastructure, a peephole, catches the cheapest wins.
  The structural win, promotion, needed cross-block liveness and could not have
  been attempted before 0.57.0.
- Promotion and coalescing are a pair. Neither is worth much alone on these
  workloads; together they cut both memory traffic and instruction count.
- A gate-off-is-byte-identical discipline makes it safe to try several ideas and
  keep only the ones that measure well.

## Status

The promotion pass is now on by default. It lives in `ir/promote.c` and covers
every scalar class on every target: word scalars and sub-word char/short
everywhere, 8-byte pointers and longs on the LP64 targets, and 8-byte i64 as a
register pair on the ILP32 targets (except MIPS, where the i64 pair loses to
register pressure and is gated off through the per-backend `promote_wide` flag).
`SKJ_NO_PROMOTE` disables it, kept as an escape hatch for measurement, A/B
comparison, and bisecting a suspected promotion bug. The debug-info interaction,
once the open blocker, is resolved: the toolchain emits line-table debugging only,
with no variable-location DWARF, so promotion cannot invalidate it, and the line
table rides on untouched `IR_LOC` markers (see the section above). Promotion is
safe under `-g` with no DWARF work.

The RISC-V machine-code peephole stays a prototype, still default off behind
`SKJ_RV_PEEPHOLE` in `backend/rv_mc_text.c`. Its measured win was small and its
main result was a lesson (constant and address reuse belong at the IR level, not
post-emit), so it did not earn the promotion the register pass did.
