# Liveness and Register Allocation

How each backend decides which temp lives in which register, why the current
liveness computation is unsound for one class of control flow, and the design
for a shared control-flow-aware liveness pass that fixes it.

## Current approach

Every backend (`backend/regalloc_*.c`) runs a Poletto-Sarkar linear scan. The
allocator needs one live interval per temp. Today each backend computes those
intervals with a single textual pass over the instruction list:

- `first_def[t]` is the position of the instruction that writes `t`.
- `last_use[t]` is the highest position at which `t` appears as `dst`, `a`, `b`,
  or an extended-asm operand.
- The interval is the single contiguous range `[first_def[t], last_use[t]]`.

The same pass also records, per backend, the register class of each temp
(integer, 64-bit pair, float), the call positions (for the float-across-call
spill), and one adjustment: an `IR_ARG` operand is consumed at the following
call, not at the `IR_ARG`, so each pending argument's `last_use` is extended to
its consuming call. This argument extension is an IR-semantics fact, not a
control-flow fact, and it stays regardless of how liveness is computed.

The five passes are near-identical copies.

## Soundness precondition

A single contiguous interval `[first_def, last_use]` describes a temp's true
live range only when that range is textually contiguous: the temp is live at
every position between its definition and its last use, and nowhere else. The
IR is designed to guarantee this. As `ir.md` states, the model is "SSA within,
memory between": each temp is written once inside a basic block, and any value
that must survive across a block boundary round-trips through a local slot with
`IR_STL` and `IR_LDL`. If every front end honors that rule, no temp is ever
live across a branch, the textual range is exact, and the linear scan is sound.

## The gap

In practice a front end can emit a temp that stays live in a register across a
control-flow edge, violating the precondition. Measured on one real function
(the Kobold `uses ns.*` wildcard scanner, `BindNamespaceWildcard` in
`kobold/kpcom.pas`), only about 1 percent of temps cross a block boundary, so
the rule is almost always honored. The rare violations split into two cases.

A **forward merge**, where an `if`/`else` reconverges, is harmless. The textual
range `[first_def, last_use]` spans the branch and is conservative: the temp is
treated as live across the whole span, so the linear scan reserves its register
for the entire region and nothing collides.

A **loop back-edge** is not harmless. Consider a value that is live across the
back-edge but whose last textual use sits before the end of the loop body. Its
textual `last_use` stops at that use, so the interval ends inside the loop even
though the value is still live on the next iteration. The allocator then hands
the register to another temp defined later in the same loop body, whose textual
range does not overlap the shortened interval. On the next iteration the
original value is read back as whatever the second temp left behind.

This is exactly what the directory-scan loop in the wildcard resolver hit: a
string pointer live across the `FindNext` back-edge was read back as `0xffffffff`
and released, faulting in `__pascal_str_release`. The base allocation happened to
place the colliding temps so the clobber did not occur. A move-coalescing
experiment that only changed which register each temp received, without changing
any live range, moved the two temps onto the same register and the latent bug
became a crash. The allocation was valid under the textual model (a verifier
found zero textual-range overlaps), which confirms the model itself is the
problem, not the register choice.

## Fix: control-flow-aware liveness

Replace the textual pass with a real liveness analysis, shared by all five
backends in `ir/liveness.c`. This is the standard input Poletto-Sarkar assumes:
live intervals derived from a backward dataflow, rather than a textual
approximation.

1. **Build a control-flow graph** from the linear instruction list. Block
   leaders are the function entry, every `IR_LABEL`, and every instruction that
   follows a terminator. Edges are: `IR_JMP` to its target; `IR_BZ` and `IR_BNZ`
   to their target and to the fall-through; `IR_RET` and `IR_RETV` to the exit;
   a fall-through otherwise. `IR_IJMP` and `IR_IJMP64` are computed gotos through
   a register, so they edge to every label whose address is taken by an
   `IR_LABADDR` or `IR_LABADDR64`. That set is conservative and correct, and it
   matters here because threaded-code dispatch is itself a loop.

2. **Solve liveness by backward dataflow** to a fixed point:
   `live_out[B]` is the union of `live_in[S]` over successors `S`;
   `live_in[B]` is `use[B]` union `(live_out[B] minus def[B])`. Big functions
   have on the order of a hundred blocks and converge in a couple of passes, so
   the cost is negligible next to the rest of compilation.

3. **Build the intervals**. `first_def[t]` stays the definition position.
   `last_use[t]` extends to the end of every block in which `t` is live-out.
   That extension across a live-out block is precisely what the textual pass
   misses at a back-edge: a value live around a loop now has its interval
   stretched to cover the loop body, so no temp defined inside the loop can reuse
   its register.

4. **Feed the existing linear scan unchanged.** It still consumes
   `first_def[]` and `last_use[]`; those arrays are now control-flow-correct.

The per-backend register classification, call-position collection, and the
`IR_ARG` to call extension stay in each backend. Only the interval computation
moves to the shared pass, which also removes five copies of the same loop.

## Validation

This changes liveness for every backend, not only for any one optimization, so
extended intervals can raise register pressure and add spills in some functions.
The change must be measured (spill and instruction counts) and run through the
full gate on all five backends, the same gate that caught the original fault.
The `IR_IJMP` edge set is the one correctness-sensitive part to get right.

Once this lands and the gate is green, the move-coalescing bias described in the
project history becomes safe to reapply on the two-address backends, because the
allocation it perturbs is then sound for any valid register assignment.

## Implementation findings

A first implementation pass exposed two problems that the design above
understates, so the shared pass is more work than a direct port of the textbook
dataflow.

First, the naive interval construction over-approximates on this IR. A temp that
is only conditionally defined (written on one arm of an `if`, read after the
merge) is reported live-out of blocks all the way back toward the entry, because
the backward solve cannot see that the value does not exist on the other path.
Its interval then stretches across most of the function and inflates register
pressure. The textual pass never had this problem, since it only ever looked at
real mention positions. A correct shared pass has to bound each interval to the
region where the temp is genuinely live, which means pruning intervals back to
the dominated def rather than taking the raw live-out closure. That needs
dominance information, not just the dataflow fixpoint.

Second, the back-edge unsoundness is not a single case. A targeted pass that
extended only loop-invariant values (defined before the loop, used inside) and
loop-carried values (used before defined within the loop) was sound for those
two shapes and passed the base gate, but re-enabling coalescing on top of it
still faulted at the same `__pascal_str_release` site. So at least one more
shape of cross-edge liveness escapes those two rules. Chasing the shapes one at
a time is whack-a-mole. The robust fix is the full dominance-bounded liveness,
not a set of special cases.

The conclusion for anyone picking this up: the fix is a genuine dominance plus
liveness pass (or, alternatively, teaching the front ends to honor the slot
round-trip strictly so no temp ever crosses an edge in a register), scoped and
measured as its own effort. The quick textual-extension shortcuts do not make
the allocation sound enough to re-enable coalescing.

## Implementation plan

A correction to the findings above sizes the work correctly: for the
single-interval linear scan the backends actually use, dominance is not required
for correctness. It is required only to keep register pressure from regressing.
The scan consumes one `[first_def, last_use]` interval per temp. That model is
sound as long as `first_def[t]` is the real earliest definition position and
`last_use[t]` covers every position where the temp is genuinely live, back-edges
included. An over-long interval is always safe; it spills, it never miscompiles.
The original fault was `last_use` ending too early at a back-edge, never
over-approximation. So the correctness fix is a correct backward dataflow, and
dominance is a later refinement that only trims the spurious pressure a
conditionally-defined temp creates.

### Phase A: sound control-flow liveness

No dominance. This is the phase that decides whether coalescing can be
re-enabled at all.

1. Build the CFG from the flat instruction list. Block leaders are the entry,
   every `IR_LABEL`, and every instruction after a terminator. Terminators and
   their edges: `IR_JMP` to its target; `IR_BZ` and `IR_BNZ` to target and
   fall-through; `IR_ADDO`, `IR_SUBO`, `IR_MULO` to `label` and fall-through only
   when `label >= 0`; `IR_RET`, `IR_RETV`, `IR_RETV64`, `IR_RETV_AGG`, `IR_RETV2`
   to the exit; `IR_TAILCALL` and `IR_TAILCALLI` to the exit; `IR_IJMP` and
   `IR_IJMP64` to every label whose address is taken by an `IR_LABADDR` or
   `IR_LABADDR64`; fall-through otherwise.
2. Gather def and use per block, mirroring the backend scan exactly: the `dst`
   definition, the `a` and `b` uses, and the `asm_info` in and out operands. This
   parity is not optional. If the shared gather differs from what the backend
   feeds the scan, the intervals describe a different program.
3. Solve liveness by backward dataflow to a fixed point:
   `live_out[B]` is the union of `live_in[S]` over successors; `live_in[B]` is
   `use[B]` union `(live_out[B] minus def[B])`.
4. Build the intervals. `first_def[t]` is the textual minimum definition
   position and is never pulled back by a live-in. `last_use[t]` is the maximum
   of the textual last mention and the end position of every block in which `t`
   is live-out. Holding `first_def` at the real definition is what keeps the
   single-interval model from blowing up on conditional defs.
5. Validate Phase A alone on x86-64 with coalescing still off: full gate green,
   spill counts recorded. Then re-enable coalescing on x86-64 and confirm the
   kpcom path (`check-kp-stdlib-free`, the wildcard scanner) is clean. That
   result is the go or no-go for the whole effort. If a correct dataflow still
   faults, the problem is not liveness and the work stops here.

### Phase B: dominance tightening, only if Phase A regresses pressure

Compute dominators (Cooper-Harvey-Kennedy iterative, no separate dominator-tree
structure needed) and prune each interval so a conditionally-defined temp does
not stay live into blocks its definition does not dominate. Skip this phase
entirely if Phase A's spill delta is within measurement noise.

### Phase C: rollout

Wire all five backends: include `liveness.h`, replace the textual interval init
with the `ir_live_intervals` call, and keep in each backend the register
classification, the call-position collection, and the `IR_ARG` to call
extension (an IR-semantics fact, layered on top of the shared `last_use` and
monotonic with it). Add `ir/liveness.c` to the Makefile. Run the full gate on
all five backends, then re-enable coalescing on the two-address backends (x86
and ColdFire) and run the gate again.

### Interface

```c
void ir_live_intervals(struct ir_func *fn, int *first_def, int *last_use);
```

Fills both arrays (length `fn->ntemps`) with control-flow-correct ranges. The
per-backend classification, call positions, and argument extension stay where
they are, so the 64-bit backends' existing pending-argument live-range extension
composes unchanged.

### Risks

The operand gather must match the backend scan including `asm_info`, checked by
an env-gated assertion that the control-flow intervals are a superset of the
textual ones on every function. The `IR_IJMP` edge set must reach every
address-taken label, since threaded-code dispatch is itself a loop. A cross-block
argument, where an `IR_ARG` value is defined in an earlier block, is rare but
must compose with the dataflow rather than being cut at the `IR_ARG`.

### Phase A result and the real blocker

Phase A was built (`ir/liveness.c`, `ir/liveness.h`) and wired into the x86-64
backend. The pass is sound: it only ever lengthens intervals, so a valid
assignment for the extended ranges is valid for the true ranges too, and it
cannot cause a clobber. It passes every standard x86-64 gate unchanged
(`check-x86-64`, the cc corpus, and the i64, ops, and fpu integration tests).

It does not yet pass the self-hosted path, and the reason is not liveness. The
pass raises register pressure in a few large kpcom functions, and that pressure
exposes a pre-existing front-end bug that the old lower-pressure allocation
happened to hide. The exact spot is `BindUnitNamed` in `kobold/kpcom.pas`:

```
for i := 0 to DepCount - 1 do
    if LowerStr(DepName[i]) = LowerStr(uname) then Exit;
```

The two `LowerStr` calls create managed-string temporaries. The Pascal front end
(`pascal/lower.c`) releases them unconditionally at the expression's merge, but
they are defined only on the in-bounds branch of the array access. On the other
branch the release runs on a temporary that was never assigned. It is harmless
while the temporary lives in a register that defaults to a release-safe zero; it
double-frees once register pressure spills the temporary into a stack slot that
is never zero-initialized.

This is a latent correctness bug independent of the liveness work. It reproduces
on the plain textual allocator with no liveness change at all, simply by lowering
the x86-64 integer register count from five to four and rebuilding the
self-hosting compiler (`make build/skj-kpc-rv`), which then faults compiling the
`uses ns.*` test. A use-after-free report (valgrind) points straight at the two
`str_release` calls at the merge.

The order of work is therefore: fix the front-end managed-string temporary
(initialize it to the empty string before the guard, or release it only on the
paths that defined it), then wire the shared liveness pass into all backends,
then re-enable coalescing. The liveness pass itself is finished and sound; it is
gated on the front-end fix, not the other way around.

### Landed

The front-end fix is in: a short-circuit `and`/`or` now releases its right
operand's string temporaries on the path that evaluated them, so the merge does
not release temporaries the short-circuit path never created. With that in
place, the shared pass is wired into all five backends (`ir/liveness.c`, called
from each `regalloc_*.c`), which removes the five duplicated textual scans. Each
backend keeps its own register classification, call-position collection, and the
`IR_ARG` to call extension. Validated across every backend: the per-target test
suites, the cc corpus on each, the Kobold corpus, the kpcom self-host, and the
i64, ops, and fpu integration tests. Coalescing is a separate follow-up on top
of this sound allocation.
