# Numbers for builders: the decimal path

Status: decided and implemented (2026-07), the pass on
approachability.md's R3 (the beginner numeric path), folding in R17
(the fixed exactness apparatus). All six decisions at the end were
confirmed and the implementation is in the tree: the `decimal` type,
the untyped decimal literal, exact printing, and the retirement of
`fixed`/`0f`/`fixed(num, den)` behind teaching hints. A first draft
kept binary s15.16 and proposed inference plus round-trip printing on
top; review killed it (binary fixed breaks base-10 *arithmetic*, not
just spelling, see below), and the note was reworked around base-10
fixed-point.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)


## The problem, remeasured

R3 said inference steers beginners into float, the most second-class
type, while fixed (the type the design wants stat math in) has the
expert-facing literal. Re-measuring against the tree found two
harder facts.

**First, the friendly path core.md promises does not exist.** "A
plain float literal in fixed context is the one spelling that may
round" is documented, but the checker rejects both `var x is fixed =
1.5` ("cannot initialize from float") and `f * 1.5` ("cannot mix
fixed and float"). Since `0f0.1` is exact-or-error and 0.1 is not
dyadic, an approximate 0.1 is not writable as fixed at all, short of
hand-computing `fixed(6554, 65536)`.

**Second, and decisive: binary fixed breaks base-10 arithmetic, and
no literal or printing design can fix that.** In s15.16, 0.1 stores
as 6554/65536, so:

    0.1 * 3 == 0.3        // false: 19662 vs 19661
    ten items at 0.1      // sum is 65540/65536: prints 1.000061

A builder pricing anything at a tenth sees penny-drift. The
"drift-free" claim means *deterministic* (every machine computes the
same integers), not *intuitive*; for an audience that thinks in
tenths and hundredths, deterministic-but-wrong-looking is the gold
counter bug class again. The first draft's shortest-round-trip
printing masks single constants and is helpless against sums.

Meanwhile the type's machine-side rationale has quietly lapsed: the
classic argument for power-of-two scaling is shift-based rescaling,
but multiply and divide already go through host helpers with 64-bit
intermediates, where a divide-by-constant costs the same as a shift
to within noise. Add, subtract, compare, and negate are plain int
ops in *any* base. The DSP/Q-format heritage serves audio and
geometry, which live at the hypervisor tier. We pay binary's
intuition costs and collect almost none of its rent. Nobody thinks
in power-of-two denominators.

## Survey

- **Inform 7**: numbers were integers-only for years; real numbers
  arrived late and opt-in. The strongest audience precedent: the
  non-programmer default is the exact, predictable kind.
- **C#, SQL, COBOL**: `decimal`/DECIMAL is the established name and
  concept for base-10 exact arithmetic, chosen everywhere the
  numbers face people (money, quantities) rather than signals.
- **Go**: untyped constants, a literal adapts to context and has a
  default type at inference boundaries. The literal mechanism this
  note borrows, with the default swapped from float64 to decimal.
- **Lua**: one number type (double); simple, wrong for cross-machine
  determinism, and 5.3 reintroduced integers. One type is not the
  answer.
- **Pascal**: `/` versus `div` is the classic answer to the integer
  division trap (noted below, deferred).
- **Doom-era 16.16**: the proven workhorse of deterministic game
  math, *for engine internals*: coordinates and angles chosen by
  programmers, not decimals typed by writers. Right tool, different
  tier; its niche here is the hypervisor/vec side, not the builder
  type.
- **BASIC's `x%`/`x!` suffixes**: the sigil way to pick numeric
  types; cautionary, and against the language's principles.

## Design

### The rule

**Whole numbers are int, decimals are decimal, float is asked for by
name.** One sentence, teachable, and the path of least resistance
lands on a type whose arithmetic does what the reader expects.

### The type: `decimal`, base-10 fixed-point, renamed

`decimal` is value x 10^-4 stored in an int32: four fraction digits,
range +/- 214,748.3647, step 0.0001.

- **Every decimal of up to four fraction digits is exact.** 0.1,
  0.25, 2.75, 0.001. `0.1 * 3 == 0.3` exactly; ten 0.1s are exactly
  1. The audience's mental model holds.
- **Range improves 6.5x** over s15.16 (+/- 32,768) in the same 32
  bits. What is lost is binary's finer 1/65536 step, which nothing
  at this tier needs; finer steps are float's job, by name.
- **Printing is exact and trivial**: divide by 10^4, strip trailing
  zeros. The first draft's round-trip printing machinery is
  unnecessary.
- **Determinism is unchanged**: exact integer math everywhere.
- **The representation trick is unchanged**: a decimal is one int32
  word, so fields, list elements, match subjects and labels,
  comparison chains, `maybe`, sends, and the R8 overflow faults all
  carry over as they stand. Only the rescale constants change.

The name: `decimal` rather than `fixed`. It is the honest,
audience-readable word (builders learned "decimals" in school;
"fixed" names an implementation strategy and can even misread as
"unchanging"), it is the established industry name for exactly this
concept, and core.md already reserved it for a base-10 type, which
this now is. `fixed` joins the removed forms with a lexer teaching
hint ("the type is called `decimal` now").

### Decimal literals become untyped decimal constants

The lexer's FLOAT_LIT is renamed in the grammar to **DEC_LIT** (same
token, new meaning): an undecorated decimal literal is an untyped
decimal constant, checker-internal type `dec`, which adapts to its
context (the Go model, scoped to one case):

- In decimal context (ascription, a decimal operand, a decimal
  parameter) it becomes a decimal constant.
- In float context it becomes float, exactly as today.
- Joining: `dec op decimal` is decimal, `dec op float` is float,
  `dec op int` and `dec op dec` stay dec.
- At any boundary where dec survives unresolved (var inference, an
  interpolation hole, a list element, a match subject or label), it
  resolves to its default type: **decimal**.

`dec` never appears in a declared type and never reaches the
lowerer: the checker resolves every literal to decimal or float, and
the decimal case folds to a scaled int32 constant at compile time.
"Cannot mix decimal and float" stays the rule for typed values; the
literal is no longer a float value, so nothing is being mixed.

    var speed = 1.5             // decimal: everything already works
    var x is decimal = 0.1      // exactly 1000/10^4
    d * 1.5                     // decimal multiply (was an error)
    var t is float = 1.5        // float by name, as before
    player.tell("${0.1 * 3}")   // "0.3", exactly
    match speed
        1.5 then ...            // decimal labels, no prefix

### Exact or an error, transplanted whole

In base 10 the exactness apparatus dissolves instead of needing a
tier. A literal with at most four fraction digits is exact by
construction, which is every decimal the audience writes. A literal
with nonzero digits past the fourth **does not fit and is a compile
error** ("decimal holds four fraction digits; 3.14159 does not fit;
write 3.1416, or use float"), so the principle survives with no
special spelling: the plain literal in decimal context is exact or
an error, and float is the documented escape for finer values.
Consequently:

- **`0f` retires.** Its reason to exist was that most decimals are
  not dyadic; in base 10 that problem is gone. The lexer answers
  `0f1.5` with a hint ("decimal literals need no prefix; write
  1.5").
- **`fixed(num, den)` retires.** Power-of-two denominators were the
  workaround for unwritable dyadic values; nothing is unwritable
  now. The lexer/checker hint points at plain literals.
- R17 is thereby resolved by dissolution, not documentation.

### Arithmetic and rounding at runtime

Add, subtract, compare, negate: plain int ops, unchanged. Multiply
and divide rescale in the 64-bit helpers (renamed `__exc_decmul` /
`__exc_decdiv`): `(a * b) / 10^4` and `(a * 10^4) / b`, with
**round half away from zero** (schoolbook rounding), documented and
deterministic. A variable zero divisor stays fallible, a constant one
is decided at compile time (a nonzero literal divisor is infallible, a
literal zero is a compile error; runtime-errors.md), and the range
check stays an OVERFLOW fault, with the fault hint updated to the new
range. Int-to-decimal conversion becomes a
multiply by 10^4 instead of a shift; an int beyond +/- 214,748 does
not fit, so the conversion site gets the same inline compare-and-
branch to an OVERFLOW trap site the divide checks use (constants
check at compile time).

### The division split, revisited (decision 5, un-deferred 2026-07)

`var half is decimal = 1 / 2` is int/int division: 0, then widened
to 0.0. The first draft deferred this to a tutorial line; review of
the prior art un-deferred it.

**Pascal's answer** was to split the operator: `/` always produces
real (even on two integers) and `div` is the explicit integer
quotient. **Python 3 proved the same design at scale**: PEP 238 made
`/` true division and `//` floor division precisely because newcomers
expect `/` to mean calculator division, and it is regarded as one of
Python 3's clearly-correct breaks. The sharpest framing of why:
children understand quotient-and-remainder division perfectly well;
what confuses them is that `/` silently *discards* the remainder.
Pascal's `div` makes the discard explicit in the spelling.

**Why `/`-always-decimal does not transplant here: range.** Pascal's
real (and Python's float) had enormous range, so `n / m` always had
somewhere to live. Our decimal holds +/- 214,748.3647, so an int/int
`/` producing decimal would fault whenever the quotient exceeds the
grid: `total_gold / players` breaks the moment gold totals pass 214k,
even though both operands and the true quotient are comfortable ints.
A `/` that faults on big-quotient division it handles fine as int is
a worse beginner experience than `1 / 2 == 0`. (The operands are not
the constraint; `__exc_decdiv`'s 64-bit intermediate could take raw
ints of any size. It is strictly the quotient.)

**The whole confusion is one cell of the matrix**: decimal/decimal is
decimal, mixed is decimal, and the untyped literal already makes
`1.0 / 2` decimal division. The only ambiguous case is int/int, and
the only place it silently betrays anyone is when its truncated
quotient immediately widens into a decimal or float context. So that
exact cell is now a **compile error** with a teaching message:

    integer division truncates: this quotient is an int (1 / 2 is 0,
    not 0.5); write a decimal operand (`1.0 / 2`) for decimal
    division, or keep the truncated quotient explicitly with
    `as decimal`

Scope, stated honestly: it is a teaching check, not a soundness
check. It fires at every implicit widening boundary (var init,
assignment, argument, return, field and const defaults) when the
widened expression's *outermost* operator is an int `/`; a quotient
buried deeper (`(a / b) + 1`) escapes, and the explicit `as decimal`
cast is the sanctioned spelling for a truncated quotient on purpose.
Int-context truncation (`var n = 7 / 2` is 3) stays legal; Pascal's
own lesson is that forcing `div` everywhere was the friction point.
`%` is not flagged: the remainder of two ints widens exactly.

**Recorded for later**: the comment tokens are ours to rework, so
`//` (today the line comment) and `///` (the trace comment) could be
reclaimed to make `//` the explicit integer-quotient operator in
Python's spelling, or a `div` word could join the `band`/`bor` tier.
Either arrives with the same migration-hint machinery as the other
retirements if content shows a real need for the integer quotient
inside decimal expressions; the compile error's hint would then name
it. Not taken now: the error plus the `as` escape covers the trap,
and moving the comment syntax is a whole-corpus migration to spend
only if the operator earns it.

### What this pass deliberately does not do

It does not make float first-class. Float's holes (fields, list
elements, `maybe float`, sends, match, chains) stop being
beginner-facing the moment decimals infer decimal, so they fill on
demand at mechanics tier. And it does not build a wider decimal; see
"The grid is the contract" below for the recorded decimal64 path.

## The numeric model, side by side (added 2026-07)

Three numeric types coexist, each simple because it refuses to be the
others: int never rounds, decimal never drifts, float never faults.
The roles in one sentence: **int counts, decimal measures, float
computes.** Decimal is best understood as a friendly currency system:
the type for prices, rates, percentages, weights, and stat modifiers,
where arithmetic must match paper arithmetic and equality must work.
Anyone doing trigonometry-shaped math uses float by name.

| | int | decimal | float |
|---|---|---|---|
| range | +/- 2,147,483,647 | +/- 214,748.3647 | +/- 1.8e308 |
| exact | always | on the 0.0001 grid | no (base-2 approx) |
| overflow | silent wrap (transitional) | fault | inf, by IEEE design |
| division | truncates; variable /0 fallible, constant /0 compile error | half-away rounds; same /0 rule | IEEE inf/nan |
| first-class | yes | yes | no (fields, lists, maybe, sends, match, chains) |

The complexity lives at the borders, and the rule is that every
crossing is exact, explicit, or loud:

- **int -> float**: implicit, always exact (a double holds every
  int32).
- **int -> decimal**: implicit but *partial*, the one genuinely novel
  seam: decimal's range is 10,000x smaller than int's, so a widening
  beyond +/- 214,748 is a compile error for constants and an OVERFLOW
  fault for runtime values. Inherent to any one-word scaled type (no
  32-bit grid can contain int32's own range); lived with, loudly.
- **decimal -> int**: explicit (`as`), truncates toward zero, cannot
  overflow.
- **int/int quotient -> decimal/float**: compile error (the division
  split above).
- **decimal <-> float**: never implicit, by design; the explicit `as`
  cast typechecks but is not lowered yet, a known gap (the one
  advertised gate that does not open).
- **int overflow**: silent wrap today, the one transitional
  incoherence against decimal's fault; aligns when the checked IR ops
  land (open-questions.md 4). The end state is one rule: exact types
  fault, float saturates.
- **float -> int**: explicit, unchecked for out-of-range doubles;
  mechanics tier.

## Why 32 bits: decimal against double

The Lua question deserves a recorded answer: Lua ran an entire
game-scripting ecosystem on one number type (double, integer-exact to
2^53), and doubles dwarf decimal's range. Why not doubles here?
Because the three properties decimal exists for are exactly the three
double gives up:

- **Exactness where the audience looks.** `0.1 + 0.2 == 0.3` and
  `0.1 * 3 == 0.3` are false in double. Python 3 sits on doubles with
  excellent shortest-round-trip printing, and `0.1 + 0.2` echoing
  `0.30000000000000004` is still its most famous newcomer confusion.
  Note fixed point carries *absolute* precision: near the top of its
  range a decimal holds 9-10 significant digits; it is a 0.0001-step
  type, not a 4-digit type.
- **Equality and `match` that work.** Decimal comparison is integer
  comparison. Matching or comparing computed doubles is a minefield
  the design already fenced off (float match subjects and chains are
  excluded).
- **Determinism that is trivially true.** Doubles can be
  deterministic across machines, but only under strict-IEEE
  vigilance, and this lineage is the danger zone: the 68881/68882
  family computed in 80-bit extended precision internally (the
  classic double-rounding drift), and a ColdFire server must match a
  future WASM client bit for bit. Integer math has nothing to audit.

The Lua counterpoint cuts differently here: Lua's double had to be
the *counting* type because Lua had no integer. Excelsior has int, so
decimal never holds a count; it only spans *measures*, and
+/- 214,748 with 0.0001 steps covers game measures comfortably. The
range seam is a mixing seam, not evidence the type is too small for
its job. (float32 offers nothing in this space: it keeps base-2
inexactness and shrinks the mantissa.)

Two weaknesses, named so the idioms are teachable: **accumulation**
(a running decimal total crossing 214k faults, so totals are ints;
gold is a count) and **small products** (`0.001 * 0.03` rounds to 0
on the grid, so stacked tiny probabilities and compounding
micro-rates are float's job, with the result stored as decimal).

## The grid is the contract: 10^4 at every width

Decided with the currency framing: **the four-digit grid is the
decimal family's defining contract, fixed across all present and
future widths.** The grid is not precision-motivated (see above); it
is the audience-visible arithmetic rule: where rounding happens, what
is exact, what prints. `1.0 / 3.0` prints `0.3333`, a number a game
shows a player, not `0.333333333`, a number a debugger shows an
engineer. SQL Server's `money` (int64 at exactly 10^4) is the
precedent for the stance.

The recorded upgrade path is **decimal64**: value x 10^-4 in an
int64, range +/- 922 trillion. Because the grid does not move, it is
a pure range upgrade: every existing value widens by sign extension
(bit-compatible, the freeze/thaw migration machinery's automatic
"widen" class), every literal and computed result means exactly what
it meant, and the int -> decimal seam disappears (int32 embeds
losslessly). skjegg's i64 IR support (register pairs, three backends)
already carries the add/sub/compare side; multiply/divide need wider
intermediates in the helpers. The trigger is evidence, not appetite:
playtest content whose *measures* (not totals; totals are ints)
overflow +/- 214k.

Why the fraction side stays put even at 64 bits, for the record: a
finer grid must keep the integer part covering int32 (the seam-killer
constraint caps any grid at 10^9), it lengthens player-facing
printing, and above all *rounding points are semantics*: every
multiply and divide rounds at the grid, so migrating persistent-world
content between grids changes computed results, deterministically but
differently, an engine-version break the same-grid upgrade never
risks. The cases that want a finer grid (probability stacking,
compound rates per tick) are computation-shaped and belong in float
by name.

## Staging

1. Grammar: FLOAT_LIT becomes DEC_LIT with the untyped-constant
   typing note; the `fixed` type keyword becomes `decimal`; FIXED_LIT
   (`0f`) and `fixed_ctor` leave the grammar (migration hints noted);
   `match_label` gains DEC_LIT; the numbers-for-builders rule lands
   in the header conventions.
2. Lexer: `fixed` and `0f` answer with teaching hints; DEC_LIT is
   the existing float token.
3. Checker: the internal dec type, adaptation and joining rules,
   default resolution to decimal, the four-digit exactness check,
   and `decimal` replacing fixed in the type table.
4. Lowering: rescale constants 10^4 replace the shifts; helpers
   renamed; the int-to-decimal range check; decimal literals fold to
   scaled constants.
5. Host: `__exc_decmul`/`__exc_decdiv` with half-away rounding and
   the carried-over zero/overflow checks; exact decimal printing in
   the tostr and trace helpers.
6. Tests: existing `0f`/`fixed()` tests migrate to plain literals
   (now exact); new tests for inference, mixing, the four-digit
   error, float by name, `0.1 * 3 == 0.3`, decimal match labels,
   and the int-conversion fault.
7. Docs: core.md's type-system section replaces the fixed paragraph
   (and its reserved `decimal(k)` note, which this fulfills);
   CLAUDE.md; approachability.md marks R3 and R17; fallible.md and
   runtime-errors.md references rename with the type.

## Implementation notes (2026-07)

- Decimal `%` came along free: the remainder of the scaled words IS
  the scaled remainder ((a x 10^4) mod (b x 10^4) = (a mod b) x
  10^4), so it lowers to the plain int modulo with the divide checks.
  Binary fixed never had `%`.
- The exactness check needs no lexer work: for a literal's `fval`
  (the nearest double to the source decimal), `scaled =
  round(fval x 10^4)` is exact within int32 range, and `scaled / 10^4
  == fval` holds exactly when the source value fits four digits.
- One unresolved-literal guard lives in the lowerer (an ET_DEC node
  reaching lowering is a checker bug); every consumer boundary in the
  checker resolves via `resolve_dec`, with choosers sharing one
  resolution point at the check_expr tail.
- Implementing the helpers exposed a latent ABI bug in start.S: the
  `__divdi3`/`__moddi3` family clobbered the C ABI's callee-saved
  d2/d3 (`__udivmoddi3` returns the remainder there). The old
  one-line fxdiv was too small for gcc to keep values live across the
  call, so it never fired; `__exc_decdiv` (quotient plus remainder)
  did. The wrappers now save and restore d2-d5.
- `decimal -> int` (`as int`) truncates toward zero (binary fixed's
  shift floored); documented here as the negative-value difference.

## Decisions (confirmed and implemented 2026-07)

1. Replace binary s15.16 with base-10 fixed-point, value x 10^-4 in
   an int32: YES. The case is arithmetic intuition at unchanged
   determinism and near-zero machine cost (the rescale lives in
   already-out-of-line helpers).
2. Rename the type `decimal`, with `fixed` and `0f` and
   `fixed(num, den)` retiring behind teaching hints: YES.
3. Literal exactness: more than four nonzero fraction digits in
   decimal context is a compile error naming the rounded spelling
   and the float escape: YES.
4. Runtime rescale rounding: half away from zero, documented: YES.
5. The int/int-division-widens check: UN-DEFERRED and implemented as
   a compile error (2026-07, after the Pascal `/` vs `div` and Python
   PEP 238 review above); the outermost-operator scope and the `as
   decimal` escape are recorded in "The division split, revisited".
   Reclaiming `//` (or adding `div`) for the explicit integer
   quotient is a recorded option, not taken.
6. `dec op int` stays dec (Go's answer): YES, with one carve-out:
   division and modulo resolve to decimal immediately (they are
   fallible producers and never stay untyped).

Added after the coexistence review (2026-07):

7. Decimal is the measure type, framed as a friendly currency
   system; exactness, equality, and free determinism are its
   contract, and double is not a substitute for it (the analysis in
   "Why 32 bits"): YES. Its two weaknesses are idioms, not bugs:
   totals are ints, and sub-grid computation (trigonometry-shaped
   math, stacked probabilities, compounding) is float's job by name.
8. The 10^4 grid is fixed across all present and future decimal
   widths ("The grid is the contract"): YES. Grid changes are
   rejected as engine-version semantic breaks; rounding points are
   semantics.
9. decimal64 (same grid, int64) is the recorded range-upgrade path,
   not taken: it waits for playtest evidence of *measures*
   overflowing +/- 214,748, and rides the existing i64 IR machinery
   when it comes.
