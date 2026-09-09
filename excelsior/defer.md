# `defer`: teardown that survives every exit

Status: decided (2026-07); implemented (D1-D6, `tests/exs_defer.exs`;
D7's `errdefer` and block form stay deferred). A control-flow pass from the backlog (backlog.md, TODO's
`defer` item). Tier: Mechanics (tier 2); `defer` is a robustness tool for the
systems author, its gentlest use (restoring a field) named in the mechanics
guide, not the tutorial (tiers.md). Relates to fallible.md / fallible-consumers.md
(the exits it survives), runtime-errors.md (the fault path it does not), and
records.md (the arena/value memory model it does not touch).

Note (2026-07): memory.md D6's transactional turn, which D4's original
rationale leaned on, was retired by the cost-model pass: a fault aborts
the turn with no rollback. D4's outcome is unchanged (`defer` still does
not run on a fault) but its reason changed, see the hard-fault section.
The dependency is gone, so `defer` no longer waits on the memory work.

`defer STMT` registers a statement to run when its enclosing block exits. The
backlog filed it as a "Go / Compact Pascal scope-exit cleanup keyword" and tied
it to the memory work. This pass finds that in Excelsior `defer` is **not** a
memory tool at all (the arena and value semantics already reclaim memory), and
its real job is the teardown half of Icon-style fallibility: a guaranteed
restore or release that survives the invisible exits a propagating fallible
creates.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

## Why `defer`, in this language

In Go and Zig, `defer` most often frees memory or closes a handle on function
exit. Excelsior removes that motivation: memory is arena-reclaimed, records are
inline values copied not owned, lists are copy-on-write, and strings are
immutable and shared (records.md). Nothing a script allocates needs a matching
free. So the Go rationale does not carry over, and the honest question is
whether `defer` earns its place here at all.

It does, for a reason specific to this language: **fallibility makes exits
invisible.** A fallible expression that goes unconsumed propagates out of the
enclosing func (fallible.md), so a func with several fallible calls has several
implicit exit points the source never spells. Restoring a temporarily-changed
state by hand then means repeating the teardown before every `return` *and*
consuming every fallible only so the teardown can run, which is exactly the
boilerplate fallible.md's propagation was meant to remove. `defer` is the
teardown that outlives all of those exits at once:

    verb sweep()
        self.busy = true
        defer self.busy = false        // runs however the block exits
        clean(self.floor)              // may fail and propagate out
        polish(self.floor)             // may fail and propagate out
        return                          // or fall through
    endverb

Whether `sweep` falls through, returns early, or a fallible call propagates out,
`self.busy` is restored. Without `defer` the author would consume each fallible
with `on fail` purely to reset the flag, re-introducing the per-exit teardown.
So `defer` is the natural complement to propagation: propagation removes the
boilerplate on the success path, `defer` removes it on the teardown path.

## What `defer` is not for: memory

The load-bearing difference from Go and Zig: **`defer` never frees memory.** The
arena reclaims a turn's allocations wholesale, a record is a value reclaimed
with its container, a list forks copy-on-write and is arena-held, and a string
is immutable and shared. There is nothing for a `defer` to free. Its statement
is a **state restore** (reset a field, decrement a depth counter, clear a
suppression) or, when such a thing exists, the **release of a resource that
lives outside the actor's arena** (a host-side lock or handle the arena cannot
reclaim for you). This keeps `defer`'s role small and answers the backlog's
"interacts with the memory work" note: it does not. `defer` and the arena are
decoupled, so `defer` needs nothing from the memory-management design and can
land on its own.

## The design

### `defer STMT`, at block exit

`defer STMT` registers `STMT` to run when the **enclosing lexical block** exits.
Block scope, not function scope (Zig and Swift, not Go): a `defer` at the top of
a verb body runs at `endverb`, a `defer` inside an `if` block runs at that
`endif`, and a `defer` inside a loop body runs at the end of **each iteration**.
Block scope is the predictable rule (the teardown pairs with the block it reads)
and it gives per-iteration cleanup for free, without Go's pile-up of loop defers
onto the function.

The statement is a single statement. A multi-step teardown is a func call
(`defer restore(self)`), so `defer` needs no `enddefer` block, matching the
one-line-guard aesthetic (then-in-if.md). It runs **at exit reading current
values** (Swift's model): `defer self.busy = false` assigns at exit, and a
per-iteration `defer release(x)` sees that iteration's `x`. This is the "it is
the same statement, just run when the block ends" mental model, the simplest to
teach.

### LIFO, so teardowns mirror setups

Multiple defers in one block run in **last-registered-first order** (Go / Zig /
Swift, universal). Teardown unwinds setup in reverse, which is what nested
acquire/restore pairs want:

    self.depth = self.depth + 1
    defer self.depth = self.depth - 1
    suppress(events)
    defer restore(events)              // runs first, before the depth decrement

### Which exits it runs on

`defer` runs on every **structured** exit of its block:

- falling off the end of the block,
- a `return` (which unwinds every block from here to the func boundary, running
  each block's pending defers LIFO as it goes),
- a `break` or `continue` (running the loop-body block's defers for the exiting
  iteration),
- a **fallible propagation** out of the block: the reason `defer` matters here.
  When an unconsumed fallible carries control out toward its consumer or out of
  the func, the intervening blocks' defers run as part of that unwind.

### The exit it does not run on: a hard fault

A **fault** (an unconsumed trap: index out of range, divide by zero, overflow,
runtime-errors.md) is not a structured exit. It reports and **aborts the
turn**, and the host supervisor decides what happens to the actor
(runtime-errors.md). `defer` does not run on that path. The reason
(revised 2026-07, when memory.md D6's rollback was retired): a fault is
a reported bug, not a control path, and running more of the author's
code during the abort of a turn already known broken is its own hazard
and its own machinery. State stands as written, so a `self.busy = true`
a defer would have undone may remain set; the fault report is the
author's signal, and the fix is the bug, not the teardown. On a
per-invocation host the faulted turn's field writes are discarded
anyway (the walker never freezes them, host-abi.md D7). So `defer` is
not a general `finally`, deliberately: it is scoped to the orderly
exits, where a manual teardown would otherwise be needed.

### A deferred statement does not itself fail

Because a defer runs during an unwind, there is no consumer waiting for a
failure it might raise. So a deferred statement must be **infallible or consume
its own failure**: `defer release(h)` where `release` cannot fail, or `defer
release(h) on fail note("leak")` where it can (fallible-consumers.md's inline
handler). A defer whose statement would propagate a failure is a compile error
naming the fix, since a failure with nowhere to go at unwind time is the one
thing the model cannot allow. A defer also may not `return` a value or itself
`break`/`continue` out of the unwind.

## What is deferred about `defer`

- **A failure-only `errdefer`** (Zig): a teardown that runs only when the block
  exits by fallible propagation, not on success, to undo a half-built setup.
  It is a real pattern in a fallibility-centric language, but it is a second
  keyword and a second rule; v1 ships one `defer` that runs on all committing
  exits, and the failure-only variant waits for a demonstrated need.
- **A block form** (`defer ... enddefer`): unnecessary, a multi-step teardown is
  a func call.

## Survey

- **Go `defer`**: function scope, LIFO, arguments captured at the defer
  statement, runs on normal return and on panic. Excelsior takes the LIFO and
  the runs-on-failure, but scopes to the block (not the function) and reads
  values at exit (not capture), which avoids Go's loop-defer pile-up.
- **Zig `defer` / `errdefer`**: block scope, `errdefer` the failure-only twin.
  Excelsior adopts the block scope and records `errdefer` as the deferred
  variant.
- **Swift `defer`**: block scope, runs at block exit reading current values, on
  every path including a thrown error. The closest model; Excelsior's rule is
  Swift's plus the fallible-propagation specifics and the fault exclusion.
- **C++ RAII / scope guards, Python `with`, C# `using`**: destructor- or
  context-manager-driven cleanup. Excelsior has no destructors (records have no
  lifecycle, no GC finalizers), so `defer` is the explicit stand-in, cleanup
  written where it belongs rather than hidden in a type.
- **Pascal `try ... finally`**: the block-structured guarantee. Excelsior
  rejected `try` blocks (fallible-consumers.md) and gets the same guarantee from
  a registered statement, which composes with early exits better than a block.

Excelsior's stance: Swift/Zig block-scoped `defer`, LIFO, run at exit on every
orderly path including fallible propagation, never on a fault,
scoped to state restore and external-resource release because the arena already
owns memory.

## Decisions (confirmed)

The seven decisions are confirmed. The implementation (registering a block's
defers, running them LIFO on each structured exit and threading them through the
fallible-propagation unwind, the infallible-or-self-consuming check, and the
no-run-on-fault path) follows; it rides the fallibility lowering (the fail-label
unwind) rather than the memory work, which it does not touch.

**D1. `defer STMT` registers a statement to run when its enclosing lexical block
exits.** Block scope, not function scope (Zig / Swift): a func-top defer runs at
`endverb`/`endfunc`, a loop-body defer runs each iteration. It runs at exit
reading current values (Swift), and a multi-step teardown is a func call (no
`enddefer` block).

**D2. Multiple defers in a block run last-registered-first (LIFO),** so teardown
unwinds setup in reverse.

**D3. `defer` runs on every structured exit:** fall-through, `return`, `break` /
`continue`, and a fallible propagation out of the block. Surviving the invisible
exit a propagating fallible creates is its central purpose, the teardown
complement to Icon-style propagation.

**D4. `defer` does not run on a hard fault.** A fault reports and aborts
the turn (runtime-errors.md; no rollback, memory.md D6 revised); running
author code during that abort would be more machinery in a context
already known broken, so `defer` is scoped to the orderly exits and is
not a general `finally`. State after a fault is as-written; the fault
report is the signal.

**D5. `defer` is not a memory tool.** The arena reclaims memory, records are
values, lists are copy-on-write, strings are immutable; a defer restores state
or releases an out-of-arena resource. It is decoupled from the
memory-management design and needs nothing from it.

**D6. A deferred statement must be infallible or consume its own failure**
(`defer act() on fail ...`), since an unwind has no consumer for a raised
failure; a propagating failure in a defer is a compile error. A defer may not
return a value or `break`/`continue`.

**D7. `errdefer` (a failure-only teardown) and a `defer` block form are
deferred** until a demonstrated need; v1 is one statement-level `defer` on all
committing exits.

## Implementation notes (2026-07)

The lowering keeps a per-function stack of registered deferred
statements, scoped per block by the statement-list lowering; a deferred
statement re-lowers at each exit site. Fall-through emits the block's
defers LIFO (skipped when the block already left through a terminator);
`return` and `fail` emit every pending defer, the return value computed
first and carried across the teardown in a slot (Swift's order);
`break`/`continue` emit down to the loop's entry height. The invisible
exits ride the fallibility machinery's one routing point: when a
fallible producer's failure would take the shared propagate path in a
fallible func, it is redirected through a lazily built cold chain, one
trampoline per pending defer, ending at the propagate label; a trap
site (a fault) is chosen at the same point and bypasses the chain,
which is D4 falling out of the existing design rather than being built.
D6 is enforced twice: structurally in the checker (`return`, `fail`,
`yield`, a nested `defer`, and a `break`/`continue` that would leave
the unwind are teaching errors, with a loop inside the deferred
statement still free to use its own), and at the propagation site in
the lowering, where a deferred statement's own unconsumed failure is
the compile error naming the `on fail` fix.
