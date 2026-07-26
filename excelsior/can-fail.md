# `can fail` is not a bool: keeping failure out of the value world

Status: implemented (2026-07), the pass on approachability.md's R14 (`can
fail` and `returns bool` are near-duplicates). D1, D3, D4, D5 are in the
tree, checker-only. D2 (the consumers) was revised and superseded by
fallible-consumers.md: the `if validate(k)` / `while validate(k)`
consumers were removed in favor of an inline `on fail` statement handler,
and `if` / `while` are now bool-only. See that note and the
implementation notes at the end.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

## The problem

A `can fail` func is a valueless fallible action (fallible.md): it either
succeeds or `fail`s, falling off the end is success, and an ignored
failure traps. But its call **types as `bool`** (typecheck.c: `t =
ty_bool`, "a can-fail call is its success"). That one convenience is the
whole of R14, and it cuts two ways:

- **Two spellings for "did it work".** `if validate(key)` (a `can fail`
  func) and `if is_valid(key)` (a `returns bool` predicate) read
  identically at the call site while being different mechanisms with
  different rules. A builder cannot tell from the call which they are
  looking at.
- **Failure becomes a value, exactly where the system says it is not
  one.** Because the call types as `bool`, failure flows anywhere a bool
  does: `var ok = validate(key)` stores it, `not validate(key)` negates
  it, `validate(a) and validate(b)` combines it, a `bool` field holds it.
  In every one of those, a `fail` has been silently reified into `false`.
  The failure-is-not-a-value principle, the spine of the fallibility
  design, is compromised at precisely this point.

There is a matching **discard asymmetry**: a discarded `is_valid(key)` is
fine (a pure bool, ignoring it is harmless), a discarded `validate(key)`
traps (an unconsumed failure). That asymmetry is correct, but while the
two forms look identical it reads as an inconsistency rather than a
consequence of two different mechanisms.

## What exists today

- `func f(...) can fail` sets `NF_CANFAIL` (parse.c); `fail` is legal
  only inside it; falling off the end is success.
- The call types as `ty_bool` (typecheck.c). So `if validate(key)`
  branches on the success bit, and the value flows into any bool
  context.
- Representation is the null word: the func returns 0 for failure,
  nonzero for success (fallible.md), so `if validate(key)` is a branch on
  that word. A bare `validate(key)` statement traps or propagates on
  failure; only the `if` (and other bool contexts) consume it.
- Canonical use, from `tests/exs_maybe.exs`:

      func validate(n is int) can fail
          if n < 0
              fail
          endif
      endfunc
      ...
      if validate(5)              // branches on the success bit
          r = r + 7
      endif

The dual nature is visible in that one line: the checker calls it a
`bool`, the runtime calls it a fallible producer that must be consumed.
Those are two different stories about the same value.

## Survey

The question is how a language spells "an action that can fail but yields
no value", and whether that is the same thing as a boolean query.

- **Icon (the model Excelsior forked).** Failure is control flow, not a
  value: an expression succeeds (produces a result) or fails (produces
  nothing and drives `if`/`while`/`every`/alternation). Icon never turns
  failure into a boolean; there is no "the success bit as a bool". A
  valueless success/failure is consumed structurally, never stored. The
  bool-typing convenience is the one place Excelsior departs from its own
  parent model, and R14 is the bill for that departure.
- **Rust `Result<(), E>` versus `bool`.** Distinct types. A fallible
  unit action returns `Result<(), E>`; a predicate returns `bool`. `?`
  propagates the former, `#[must_use]` warns on discarding it, and you
  cannot store a `Result` where a `bool` is wanted without unwrapping.
  The two never blur, because the fallible one is not a boolean.
- **Go `error` versus `bool`.** Two idioms: `ok := m[k]` (a boolean
  comma-ok) versus `err := do()` (an error to check). The language does
  not conflate them; the split is by whether failure carries information
  and must be handled. Go leans on lint (`errcheck`) for the discard rule
  that Excelsior gets to enforce in the type system.
- **Swift `throws -> Void` versus `-> Bool`.** A throwing action with no
  value is `func save() throws`; a query is `func isValid() -> Bool`.
  `try` marks the call, and the two are not interchangeable.

The consensus is uniform: a fallible valueless action and a boolean query
are different kinds, and no surveyed language lets the fallible one
masquerade as the boolean. Excelsior's own Icon lineage is the sharpest
witness. The fix is to stop the masquerade, not to merge the two.

## Design

**Take the bool-typing away; `can fail` stays purely in the fallibility
system.** A `can fail` call is a valueless fallible producer, not a
`bool`. It is consumed the way every other fallible thing is:

- **bare statement** (`validate(key)`): trap on failure, or propagate out
  of an enclosing fallible func. Unchanged.
- **`else`** (`save() else recover()`): run the right side on failure.
  This starts working under this change, since the operand is now
  fallible; today it is a bool and `else` does not apply.
- **`if` / `while`** (`if validate(key)`, `while connect()`): the
  valueless analog of `if var` / `while var`. It takes the then-branch
  (or loop body) on success and the else-branch (or exit) on failure,
  consuming the failure so nothing traps. Spelled exactly as today, but
  recognized by the operand being a `can fail` call, not by typing it
  `bool`.

And the value uses stop being legal: `var ok = validate(key)`, `not
validate(key)`, `validate(a) and validate(b)`, and storing a call in a
`bool` field are compile errors. Each gets a teaching message pointing at
the honest choice: if you want a boolean fact to store or combine, call a
`returns bool` predicate; if you want a failure to handle, consume it
with `if` / `else` / a bare statement.

**Keep both `can fail` and `returns bool`; they are genuinely different
kinds.** The guidance, one sentence each:

- `returns bool` is a **query**: it computes a boolean fact the caller
  will branch on, store, negate, or combine. Ignoring it is harmless.
- `can fail` is a **signal**: success or failure in the fallibility
  system, meant to propagate, be caught by `else`, or trap if dropped.
  It is not a fact to store.

They coincide only at `if f(key)`, and even there they now differ in
kind: one tests a value, the other consumes a failure, and the checker
knows which. Everywhere else, storage, combination, negation,
propagation, and discard, they diverge, and the divergence is no longer a
trap for the reader because the value one can never pretend to be the
other.

The `validate` example in fallible.md is worth revisiting under this
lens: a pure `n < 0` check is really a query, so it reads better as
`returns bool`. `can fail` earns its keep on an action whose failure
should *flow*, `save`, `connect`, `reserve`, where dropping the failure
is a bug the trap should catch. Re-spelling the doc example to such an
action (D4) removes the blur at the source.

### The considered alternative: drop `can fail` entirely

The other half of R14's recommendation: delete `can fail`, and let a
valueless "did it work" be a `returns bool`. It is simpler on the surface
(one keyword gone, one spelling for the query), but it pays for that:

- **Enforced handling is lost.** A `bool` may be ignored, so a forgotten
  check becomes a silent bug, exactly the class the fallibility system
  exists to catch. For the beginner audience this is the wrong trade.
- **Propagation is lost.** A `bool` does not flow Icon-style; a validator
  whose failure should bubble out of the caller must be rewritten as `if
  not ok then fail`, hand-plumbed at every level.
- **The system fractures.** Value-returning fallibles stay enforced
  (`returns maybe T`, indexing, divide), valueless ones do not. A
  fallibility system that covers values but not actions is a
  half-system, and the asymmetry is its own new trap.

So `can fail` is kept (D5); the fix is to stop it impersonating a bool,
not to remove it.

### The principle

Failure is consumed, never stored. A `can fail` result may be branched on,
fallen back from, or propagated, but it may not be captured into a value,
because the moment it becomes a `bool` it has left the fallibility system
that gives it its guarantees. `returns bool` is for facts; `can fail` is
for signals; the checker keeps them apart.

## Staging

Checker-only, small; the runtime is already right (the func returns the
0/1 success word, which the valueless consumers branch on unchanged):

1. **Retype the call.** A `can fail` call is no longer `ty_bool`; it is a
   valueless fallible producer (its own internal marker, akin to the
   `maybe` producers).
2. **Recognize the valueless consumers.** `if` / `while` accept a bare
   `can fail` call as a fallible consumer; `else` applies to it; a bare
   statement traps or propagates (already so). Reject it in every value
   context with a teaching error.
3. **Docs.** Re-spell the fallible.md example as an action, and add the
   query-versus-signal sentence to fallible.md and CLAUDE.md.

## Decisions (confirmed)

**D1. Remove the bool-typing of a `can fail` call.** The call is a
valueless fallible producer, not a `bool`. This is the whole correctness
content of the pass: failure stops being a value.

**D2. Valueless `if` / `while` / `else` consumers.** `if validate(key)`
and `while connect()` are the valueless analogs of `if var` / `while
var`, recognized by the operand being a `can fail` call; `else` applies
to a `can fail` operand. The surface is unchanged; only the typing behind
it is honest.

> Revised and superseded by fallible-consumers.md (implemented): reusing
> the boolean `if` / `while` to consume a success/failure outcome is
> itself an axis slip (false and failure collapse in a `maybe bool`).
> That note replaced this consumer with an inline `on fail` statement
> handler and made `if` / `while` bool-only; `if validate(k)` no longer
> works. The rest of R14 (D1, D3, D4, D5) stands.

**D3. Reject `can fail` in value contexts, with teaching errors.**
Capturing into a variable or `bool` field, negating, or combining a `can
fail` call with `and`/`or` is a compile error that points to the `returns
bool` predicate (for a fact) or the fallible consumers (for a signal).

**D4. Keep the guidance and re-spell the example.** Document
query-versus-signal in one sentence each, and change the fallible.md
`validate` example from a pure check to an action whose failure should
flow, so the doc stops modeling the blur.

**D5. Keep `can fail`; do not drop it for `bool`.** Dropping it loses
enforced handling, Icon-style propagation, and leaves the fallibility
system covering values but not actions. The half-system is a worse
outcome than two honest kinds.

## Implementation notes

Checker-only (typecheck.c, plus `ET_SIGNAL` in excelsior.h). The runtime
was untouched: a `can fail` func already returns the 0/1 success word,
which `lower_cond` tests for `if`/`while` and `fail_if_zero` tests for a
bare statement, all structural on `NF_CANFAIL`, so no lowering changed.

- **D1.** A `can fail` call now types as the new singleton `ty_signal`
  (`ET_SIGNAL`), not `ty_bool`. That single retype is what stops failure
  being a value.
- **D2 (`if`/`while`).** `want_bool` (the `if`/`while` condition check)
  accepts a `ty_signal` operand and returns: `if validate(k)` is the
  valueless consumer, and `lower_cond` already branches on the success
  word, so the lowering was unchanged. `if var`/`while var` on a signal
  gets a targeted message ("no value to bind; drop `var`").
- **D3.** A `no_signal` helper rejects a signal in value position with
  the teaching error, called on both operands of a binop (`and`/`or`/
  `==`/`<`/`+`), on a unary operand (`not`), and at the six value-to-slot
  boundaries (argument, `var`, assignment, `return`, field default,
  `const`). Exotic contexts (a match subject, an index, a `then`
  condition) fall through to the ordinary type error, which still rejects
  and now reads "got a can-fail signal".
- **D4.** fallible.md's example is re-spelled from the pure-check
  `validate` to the action `take`, its "types as bool" bullet corrected
  to the signal/query-versus-signal wording, and CLAUDE.md updated.

Resolved (fallible-consumers.md): **the `else` clause of D2.** A fallback
on a valueless action was left deferred here, then settled in the axis
revision: `else` stays value-only, and a valueless `can fail` action is
handled by the new statement-level `on fail` instead. `else`-on-signal is
rejected with a message pointing to `on fail`, not a "not supported yet"
placeholder.

Verified by hand across the six directions (capture, negate, combine,
`if var`, `else`, and the valid bare / `if` / predicate forms); the
existing `if validate(5)` in tests/exs_maybe.exs still checks and runs.
check-exc 45/45.
