# Runtime errors that teach: the trap UX

Status: decided and v1 implemented (2026-07), the pass on
approachability.md's R1, with R2 (the silent match no-op) and R8
(arithmetic failure) riding along as the priority section recommended.
All six decisions at the end were confirmed; the v1 scope is in the
tree (per-site descriptors, fallible divide/modulo, the divide-overflow
and decimal-helper OVERFLOW faults, the message catalog in the test host,
and the -t no-match trace event).

Made by a machine. PUBLIC DOMAIN (CC0-1.0)


## The problem

"Errors teach" is a design principle, but it stops at compile time. The
entire runtime failure story today is:

    trap: no branch chosen and no else

written to stderr by `__exc_trap`, followed by exit 70. No file, no
line, no failing value, and not even the failure kind: an out-of-range
index prints the same line as a match with no arm. For the audience
this is worse than a compile error, because it happens while the
content is live. Two adjacent findings share the fix: a `match`
statement with no `else` and no matching arm is a silent no-op (R2),
and arithmetic is not handled at all (R8): `10 / 0` is a hardware fault
under the VM (SIGFPE under qemu today), `INT_MIN / -1` is
target-dependent, and `INT_MIN * -1` silently wraps.

## What exists to build on

Facts of the current implementation that shape the design:

- **Trap sites converge and lose their identity.** All fallible
  producers in a plain func or verb branch to one shared per-function
  fail label (`fn_fail_label` in lower.c), which calls `__exc_trap`.
  The chooser tail (`lower_else_or_trap`) is the second emit site. By
  the time the trap runs, which site failed is gone. Any design must
  reintroduce per-site identity on the failure path without touching
  the happy path.
- **The lowerer has the source span.** Nodes carry `line` (`lerr`
  reports `file:line` during lowering), so each check site can name its
  location at compile time.
- **The descriptor machinery exists.** `intern_strlit` plus
  `init_ivals`/`init_syms` globals plus `IR_LEA` is exactly how string
  literals and list constants are emitted; a trap descriptor is the
  same shape.
- **The host already knows the verb context.** At every `__exc_send`
  entry the host holds the receiver's class and the selector, so "in
  verb on_open of BarrowChest" costs nothing to report. No frame
  walking is needed for one level of context.

## Survey

- **8-bit BASIC**: `?SUBSCRIPT OUT OF RANGE ERROR IN 130`. The
  beginner gold standard: kind plus location, always on, no
  configuration. Proof that location is the minimum viable payload.
- **LambdaMOO**: the direct ancestor. A runtime error produces a
  traceback (verb and line) delivered to the *programmer/owner* of the
  verb, never raw to the player; errors are values catchable with
  try/except. The routing lesson: the author sees the trace, the
  player sees the action fail politely.
- **Turbo Pascal**: `Runtime error 201 at xxxx:yyyy`, mapped to a
  source line by the IDE. Two lessons: an address-to-line map
  reconstructs location without inline strings, and range checks that
  are opt-in (`{$R+}`) are checks nobody has on when it matters.
  Excelsior's bounds checks are always on; keep that.
- **Lua**: per-chunk line tables keyed by instruction index; `error()`
  prepends `file:line`. The compact PC-to-line precedent.
- **Inform 7**: runtime problems are named and numbered (`P21`) with a
  documentation anchor per problem. Numbering gives errors a stable
  handle to search and to link from a builder client.
- **Erlang**: crash reports are structured terms routed to a logger;
  the process dies, the supervisor decides. The actor analog: a trap
  aborts the turn and the host owns what happens next.
- **Elm**: eliminates runtime errors by construction. Excelsior's
  fallible-with-`else` is the analog; traps are the residue that
  remains, which is exactly why the residue must teach.

## Design

### Fault taxonomy

One word for all of it: a **fault** is the script failing at runtime.
Compiled traps and host-detected faults share one report format.

Compiler-emitted (each a trap-descriptor kind):

    NO_BRANCH     match/select expression chose no arm, no else
    INDEX_RANGE   index out of range (carries index and length)
    UNCONSUMED    a fallible expression failed at a plain boundary
    DIV_ZERO      integer / decimal divide or modulo by zero (R8, below)
    OVERFLOW      an int or decimal operation with no representable
                  result: INT_MIN / -1, INT_MIN % -1, and add /
                  subtract / multiply / negate overflow (R8, below)

Host-detected (no descriptor; the host builds the report):

    TICK_BUDGET   the per-task tick budget ran out (runaway loop)
    STACK         VM stack exhaustion
    FAULT         anything else the VM raises (the SIGFPE class,
                  which the R8 checks remove for integer math)

### The trap descriptor

Per trap-bound site the compiler interns one small constant global,
emitted with the existing `init_ivals`/`init_syms` machinery:

    struct exc_trapdesc {
        int         kind;    /* EXC_TRAP_* */
        int         line;
        const char *file;    /* interned, one per module */
    };

and the trap call gains arguments:

    void __exc_trap(const struct exc_trapdesc *why, int a, int b);

`a` and `b` are kind-specific runtime words: the index and the length
for INDEX_RANGE, the subject value for NO_BRANCH, zero otherwise. They
are loaded on the failure path only (the values are already in slots at
the check sites; `index_check` spills both).

### Lowering: per-site cold trampolines

The happy path does not change: one compare and one branch per check,
exactly as today. What changes is the branch target. A fallible
producer whose current fail label is the statement default in a
non-fallible func gets its own cold trampoline instead of the shared
label:

    Lfail_17:                       ; cold section
        lea   __exc_trapdesc_17, arg0
        move  <index slot>, arg1
        move  <len from descriptor>, arg2
        jsr   __exc_trap

Consumed failures need nothing: when `cur_fail` is a consumer's label
(`else`, `if var`, `while var`, capture), the branch target is the
consumer and no descriptor is emitted. The lowerer knows which case it
is at emit time, so descriptors exist only for sites that can actually
trap. The shared `fn_fail_label` survives only in fallible funcs, where
it propagates (returns the null word) rather than trapping.

Cost: a handful of instructions per trap-bound site, in cold code, plus
one small descriptor global each. In-world scripts are short; this is
noise. The chooser tail (`lower_else_or_trap`) is the same change with
the subject value as the argument.

### Propagated failure loses the cause; report the boundary

In a fallible func, `return xs[i]` failing propagates to the caller,
and the trap (if any) happens at the caller's plain boundary, naming
the call site rather than the failed index inside the callee. That is
the correct v1 reading: the trap names where the contract was broken
(a failure nobody consumed), which is where the fix goes. A later
refinement can thread the original cause through a reserved global
(`__exc_fail_why`, stored by the producer's cold path, read by the
trap) to add one "caused by" line; the null-word ABI does not change.

### The message: kind, location, values, hint

The host owns formatting; the descriptor owns facts. Each kind has a
message and a teaching hint, in the same voice as the compile errors:

    fault in verb on_open of BarrowChest
      chest.exs line 12: index 4 is out of range (the list has 3 elements)
      hint: give the index a fallback with `else`, or branch with `if var`

    fault in verb greet of Innkeeper
      inn.exs line 31: match chose no arm (the subject was 7) and there
      is no `else`
      hint: add an `else` arm, or cover 7 with a label

The first line comes from the host's send context (class and selector),
not from the descriptor. The test host prints the report to stderr and
keeps exit 70; `run-exc-tests.sh` needs no change, and a test asserting
a specific fault can diff stderr later if wanted.

### Routing in the real host

The MOO rule, restated for the actor model: a fault report belongs to
the **author channel, never the player**. The trap hypercall gives the
host the descriptor; the host attaches the actor, class, verb, and turn
id, and routes the report to the owning builder's trace channel (the
same channel the static introspection index feeds). The player-facing
result is only that the action did not complete. What "did not
complete" means for actor state mid-turn (abort and roll back the turn,
or keep the partial writes) is the turn-model transactionality
question, owned by the brief and host-abi.md, not this note; the report
format is the same either way.

### R8: arithmetic faults, the whole family

Divide by zero is not the only way integer math fails. The inventory
for 32-bit two's complement:

- **Zero divisor**: `x / 0`, `x % 0`. A domain condition; the caller
  often knows what an absent quotient means (`total / count`).
- **Divide overflow**: `INT_MIN / -1`, and `INT_MIN % -1` (the same
  operation underneath): the true quotient 2^31 is unrepresentable.
  Hardware disagrees about it: x86 raises the same #DE as divide by
  zero, WASM traps, RISC-V defines the result as INT_MIN, and ColdFire
  sets the overflow flag and leaves the destination unchanged. The
  ColdFire-server / WASM-client pairing in the brief therefore needs an
  explicit check at this site no matter which semantics we pick;
  determinism does not come free from any target.
- **Silent overflow**: `INT_MIN * -1`, add or subtract past the range,
  negating INT_MIN. No target faults; every target wraps. If the
  language does nothing, a gold counter quietly goes negative, the
  classic game bug the audience cannot diagnose.

The proposal splits the family by a principle the design already uses
elsewhere: **a domain condition is fallible, a bug is a fault.**

**The zero divisor is fallible.** "There is nothing to divide by" is
the arithmetic sibling of "there is no element 4": an expected absence
the consumers already handle well:

    var rate = total / count else 0     // the natural spelling
    if var avg = sum / n ...            // branch on it
    var q = x / y                       // unconsumed: DIV_ZERO fault

The lowerer emits a zero test on the divisor branching to `cur_fail`,
then the divide. One compare before an already-expensive divide is
noise, and the checker change is small (division joins indexing as a
fallible form). A **constant divisor is decided at compile time** rather
than left fallible (implemented, promoted from the peephole this once
was): a nonzero literal divisor cannot fail, so `a / 2` is infallible and
takes no `else` / `if var` (`10 / 2 else 0` is now an error, the left
cannot fail, not merely pointless); a literal-zero divisor (`x / 0`) is a
guaranteed fault and is reported as a compile error at its site, better
than deferring it to runtime. Only a variable divisor stays fallible.

**Every unrepresentable result is an OVERFLOW fault**, not a fallible
failure and not a wrap: divide overflow, multiply, add, subtract,
negate. Two reasons overflow must stay out of the failure channel.
First, failure means "no result by design" (fallible.md: failure is
not a value); an overflow deep inside a fallible func propagating out
as that func's `nothing` would conflate "not found" with "the math
broke", and the consumer would silently handle the wrong one. Second,
`else` as saturation (`a * b else max`) reads nicely exactly once and
then licenses wrap-adjacent habits everywhere. A fault report teaches
instead:

    fault in verb grant_gold of BarrowChest
      chest.exs line 9: the multiply overflowed (2000000000 * 2 does
      not fit an int)
      hint: ints hold about +/- 2.1 billion; use smaller units or cap
      the value first

Checked arithmetic follows the Turbo Pascal lesson from the survey:
always on, never an opt-in build flag. The rare mechanics-tier need
for wraparound arrives later as named intrinsics in the `band`/`bor`
family (`wrap_add`, ...), not as the default semantics.

Cost and staging, honestly: the divide-family checks (zero divisor,
INT_MIN / -1) are explicit compares needing no IR change, and the
decimal multiply/divide helpers already hold a 64-bit intermediate, so
their range check is nearly free C. But add/subtract/multiply/negate
checks want the target's overflow flag (one `bvs` to the cold
trampoline per operation on ColdFire), and the IR has no
overflow-checked ops to express that; synthesizing the check from
plain IR costs several instructions per operation. So OVERFLOW checks
for the silent family land when the IR grows checked variants (an
ir/ design decision, recorded in open-questions.md); the divide
family and the decimal helpers land in v1. Until then `INT_MIN * -1`
keeps wrapping, documented as a known gap rather than a semantic.

Float is untouched throughout (IEEE gives inf/nan; float is
mechanics-tier). Shift semantics belong to the bitwise-tier design.

### R2: the silent match gets a trace event

The match *statement* with no `else` and no matching arm stays a no-op
in release, per case-select.md. But when tracing is on, the no-match
path emits a trace event through the existing trace channel:

    trace: inn.exs line 31: match chose no arm (the subject was 7)

The lowering is the same descriptor plus a `__exc_trace_nomatch(desc,
subject)` call on the fall-through path, compiled only when tracing is
enabled, so release builds keep the documented no-op at zero cost. This
turns the classic "ran and nothing happened" dead end into a visible
line during authoring, without changing the language rule.

### Tracebacks: deferred, and what they would need

One level of context (verb, from the host) plus the trap site covers
the audience's scripts, which the brief wants short. A full traceback
needs a return-address-to-function map (a code-range table the linker
or loader builds; skj-ld's map file is the seed) and a frame walk.
Defer until content shows deep call chains; nothing in this design
blocks it.

### The tiers and content addressing

The IR interpreter tier reports through the same host call with the
same descriptors (it holds the IR and spans directly, so fidelity is
equal or better). Descriptors are constant data compiled into the
module, so they are content-addressed with the code and survive hot
swap; a swapped verb's new code carries new descriptors. `file` plus
`line` refer to the source the hash names, so a builder client can
always show the exact line even after later edits.

## What lands in v1, and what waits

In: the descriptor and the three-argument `__exc_trap`, per-site cold
trampolines in non-fallible funcs, the chooser tail with the subject
value, INDEX_RANGE / NO_BRANCH / UNCONSUMED kinds, DIV_ZERO via
fallible divide and modulo, the OVERFLOW fault for divide overflow
(explicit compare) and for decimal multiply/divide (range check in the
64-bit helpers), the host-side message catalog with hints in the test
host (stderr, exit 70), and the trace event for the no-match
statement.

Waiting: OVERFLOW checks for add/subtract/multiply/negate (blocked on
overflow-checked IR ops, an ir/ decision in open-questions.md; until
then the silent family wraps as a documented gap), the
`__exc_fail_why` cause line, tracebacks and the code-range map, the
real host's author-channel routing (owned by host-abi.md), turn abort
semantics (owned by the brief), and any fault numbering (the Inform 7
anchor idea) until there is documentation to anchor to.

## Implementation notes (2026-07)

- The trap-bound test in the lowerer is `cur_fail == fn_fail_label`
  in a non-fallible member; consumers install their own labels and
  need no descriptors. The shared fail label survives only in fallible
  funcs (the propagate epilogue).
- Implementing the per-site branches exposed a latent bug: label ids
  start at 0 and `fn_fail_label` used 0 as its unset sentinel, so the
  first statement's default label churned and could dangle. The
  sentinel is now -1. No earlier test emitted a default-fail branch in
  a member's first statement, which is why it never fired.
- The decimal-helper faults report without file/line (a static
  descriptor in the host names the helper instead); threading a
  descriptor argument through `__exc_fxmul`/`__exc_fxdiv` is the
  refinement path if content needs located decimal overflows.
- Inference never captures: `var x = a / b` (or `= find(...)`) infers
  the payload type and traps/propagates on failure; storing the
  outcome stays the explicit `is maybe T` spelling. The checker
  strips maybe when inferring a var's type.

## Decisions (confirmed and implemented 2026-07)

1. The zero divisor is a fallible failure (R8): YES, implemented for
   int and decimal divide and modulo. The alternative (trap always,
   no `else` composition) is strictly less useful for the same check
   cost.
2. Overflow is a checked OVERFLOW fault, neither fallible nor defined
   wrap (R8): YES. Divide overflow (`INT_MIN / -1`, `INT_MIN % -1`)
   and the decimal-helper range checks are implemented; the silent
   family (add, subtract, multiply, negate) lands when the IR grows
   overflow-checked ops; wraparound arithmetic returns later as named
   mechanics-tier intrinsics. This replaced the first draft's
   "overflow stays defined wrap" recommendation, per review: the
   drift-free and exact-or-error principles argue that a wrapped gold
   counter is a bug the language should catch, not a semantic.
3. The trace-event answer to R2: YES, implemented as
   `__exc_trace_nomatch` emitted only under -t; the statement form
   stays a release no-op per case-select.md.
4. Descriptor granularity: per-site, implemented (one small constant
   global per trap-bound site; consumed failures emit nothing).
5. UNCONSUMED names the failing producer: YES, implemented; the
   descriptor carries the callee (`find`, a maybe variable's name)
   when known.
6. The overflow-checked IR ops themselves (shape, which backends get
   flag-based lowering) are an ir/ decision, tracked in
   open-questions.md; this note only depends on their existence.
