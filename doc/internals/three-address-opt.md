# Optimization Opportunities on the Three-Address Backends

What is worth optimizing next on the three-address targets (RISC-V, MIPS,
AArch64), measured from real generated code rather than assumed. The short
answer: move-coalescing, the win that was just landed on the two-address
targets, does not apply here, and the next gains come almost entirely from
cutting load and store traffic.

## Why this question comes up

The move-coalescing bias in the linear scan removes the register-to-register
copy that a two-address ISA emits to set up `dst = a op b` as `mov dst,a; op
dst,b`. ColdFire and x86-64 pay that setup move on nearly every binary
operation, so coalescing them away is a real saving (ColdFire cc corpus
reg-to-reg moves down 38 percent, x86-64 kpcom down 44 percent, ColdFire retired
instructions down up to 5 percent).

The three-address ISAs emit `op dst,a,b` with no setup move. The only
coalescable register-to-register move left is an explicit `IR_MOV`, and the
front ends emit almost none of those. So coalescing has nothing to bite on here,
and the question is what does.

## What the generated code actually spends instructions on

Instruction mix across the full cc corpus (112 programs), per target, as a
fraction of all emitted instructions:

```
                         RV        AArch64     MIPS
  loads + stores        ~53%        ~63%       ~47%
  immediate / address    ~13%        ~13%       ~14%
  register-to-register     6%          6%         6%
```

The register-to-register move share is about 6 percent on all three, and most of
those are calling-convention argument and result shuffles, which a copy hint
cannot reach. This is the direct measurement behind "coalescing does not help
here".

Memory traffic is the whole game. Half to two thirds of every program is load
and store. Breaking those down by base register:

```
  RV       loads 88% frame-relative      stores 88% frame-relative
  AArch64  loads 57% frame-relative      stores 88% frame-relative
```

On RISC-V, nearly half of all instructions emitted are frame-relative loads and
stores: the round-trip of a value out to a stack slot and back. On AArch64 the
non-frame loads are mostly literal-pool reads, covered under immediates below.

### Where the frame traffic comes from

The IR model is "SSA within a block, memory between blocks": a temp is written
once inside a basic block, and any value that must survive a block boundary is
round-tripped through a local slot with `IR_STL` and `IR_LDL`. Three things then
produce the frame traffic:

1. Every cross-block-live value is stored and reloaded at least once by
   construction. This is the structural tax of the memory-between-blocks model.
2. Within a block, the same slot is often reloaded several times with no
   intervening store, and a value is sometimes reloaded on the instruction right
   after it was stored. These loads are redundant.
3. Spilled temps add their own load and store traffic.

Sizing the within-block redundancy on RISC-V: of 8465 frame loads, a loose upper
bound of about 3800 are reloads of a slot already loaded earlier in the same
function, and 149 are the strict case of a load reading back the slot that the
immediately preceding instruction just stored. The realistic recoverable figure
sits between those two bounds, in the low thousands of instructions out of about
24000.

### Immediates and addresses

Constant and address materialization is the other large block, about 13 to 14
percent:

- RISC-V and MIPS spend it on `li` (about 12 percent) plus `la` (about 1
  percent).
- AArch64 materializes every address and large constant as a PC-relative
  literal-pool load. There are 3036 such `ldr xN, =symbol` reads in the corpus,
  about 13 percent of all instructions, and `adrp` is not used at all.

These values are frequently re-materialized within a single block instead of
being kept in a register and reused.

## Opportunities, ranked by payoff over effort

### Tier 1: the structural wins, now unblocked

1. Local redundant-load elimination and store-to-load forwarding. Within a
   block, drop an `IR_LDL` that reloads a slot already held in a register, and
   forward a just-stored register instead of reloading it. This is block-local,
   needs no dominance information, and is sound today. It directly attacks the
   within-block share of the frame traffic. Low effort, immediate payoff, and
   the natural first pass.

2. Scalar register promotion for address-not-taken locals. Keep a local in a
   register across its whole live range instead of round-tripping it through a
   slot. This is the structural lever: it removes the memory-between-blocks tax
   at the source rather than cleaning up after it. It became reachable only now,
   because it needs cross-block liveness, which `ir_live_intervals` provides (see
   `liveness.md`). Highest payoff and highest effort, sensitive to register
   pressure (RISC-V has 11 integer registers, AArch64 10, MIPS fewer after
   reserves), and it needs a spill fallback. Done as a shared IR-to-IR pass it
   would help all five backends, not just these three.

### Tier 2: moderate payoff

3. Constant and address reuse. Reuse a materialized constant or address within a
   block instead of emitting it again, and fold small immediates into the
   consuming instruction where the ISA allows. On AArch64, prefer `adrp` plus
   `add` for an address, which touches no memory and can fold into the
   following load or store.

4. Addressing-mode folding in instruction selection. Fold an address
   computation (`IR_ADD`, `IR_LEA`) into the memory operation it feeds. This is
   largest on AArch64, which has scaled-index and pre- and post-index writeback
   modes that are currently unused. It is smaller on RISC-V and MIPS, whose
   memory operands are a register plus a small immediate only.

### Tier 3: cheap and broad

5. A post-emit peephole pass. This is the cheapest new infrastructure and a
   natural home for many small wins: redundant moves, a branch to the next
   label, a doubled sign extension (AArch64 `sxtw` is about 1 percent),
   materializing zero into the hardware zero register, and compare-and-branch
   fusion. Low effort, modest but wide payoff, and a good place to land several
   of the smaller items above.

### Tier 4: allocator quality, helps the two-address backends too

6. Interval tightening. The current live intervals are single contiguous ranges,
   which over-approximate a conditionally-defined temp and inflate register
   pressure, causing spills, which are themselves frame traffic. Tightening the
   intervals cuts spills on every backend. This is the Phase B follow-up
   described in `liveness.md`.

7. Better spill heuristics. Belady next-use victim selection and live-range
   splitting, to reduce the spill share of the frame traffic that register
   promotion cannot remove.

## What keeps these in reach

The IR has no phi nodes and no dominance tree, and there are no optimization
passes today, so none of this builds on existing infrastructure. What makes the
list tractable is:

- The local passes (items 1, 3, 4, 5) need nothing new. They are single walks
  over each basic block or over the emitted stream.
- The cross-block passes (items 2 and 6) need cross-block liveness, and that now
  exists as a shared, control-flow-aware pass.
- The self-hosting constraint keeps the bar sensible. The compiler compiles
  itself, through `skj-cc` and `skj-pc`, so a new pass must be plain C with no
  constructs the self-host backends reject, and must stay simple enough to trust
  without a separate verifier.

## The honest framing

The real wins here are mostly not specific to three-address targets. Items 1, 2,
3, and 6 would help ColdFire and x86-64 as well. Move-coalescing was the one
genuinely two-address-only optimization, which is exactly why these three
backends have nothing analogous left: their next gains come from cutting load
and store traffic. The single highest-value item, register promotion, is worth
calling out because it only became reachable once the IR gained cross-block
liveness.

## Recommended starting point

Item 1, local redundant-load elimination and store-to-load forwarding. It is the
lowest-risk first pass, it is sound without dominance, it pays off immediately on
the dominant instruction class, and it establishes the per-block analysis
scaffolding that register promotion later builds on.
