# Fallible expressions and maybe T

Status: decided and implemented, v1 (2026-07). The fallibility concept accumulated
consumers for a month: `xs[i] else d`, the trap in `select`/`match`
without `else`, and the recorded designs for `if var`, `while var`, and
sources (else-operator.md). This study settles what fallibility is, so
all of them stand on one definition. Decisions confirmed: the full
synthesis (maybe T as storable capture over Icon-style expression
fallibility) and the type-carried spelling (`returns maybe T`, `can
fail`, the `fail` statement). It resolves else-operator.md's open
questions: absence of a result is fallibility, nil-coalescing is
dropped, and a bare fallible operation traps.

Note (2026-07): the value fallback operator spelled `else` throughout
this note was later renamed to `otherwise` (fallback-words.md), so `else`
means only a boolean branch. Read every `A else B` / `xs[i] else d` below
as `A otherwise B` / `xs[i] otherwise d`; the semantics are unchanged.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)


## The model

A fallible expression either produces its value or fails. In expression
flow, failure is never a value: it propagates outward, Icon-style, left
to right, aborting at the first failure, until a consumer catches it.
The consumers:

    var x = xs[i] * 2 else 0            // else: supply the default
    if var i = find("or", line)         // if var: branch and bind
    while var i = next(c)               // while var: loop until failure
    for i in source                     // for in: sugar over the pump
    var kept is maybe int = find(s, t)  // capture: store the outcome

`maybe T` is the storable capture of the same concept: a type holding a
`T` or `nothing`. Reading a maybe back is itself a fallible expression,
so the same consumers unwrap it (`kept else 0`, `if var i = kept`). A
fallible expression reaching a plain-T boundary with no consumer traps
(`__exc_trap`, exit 70), which is what `select` and `match` without
`else` already do; inside a fallible func it propagates out as the
func's own failure instead (one level of Icon, see below).

`nothing` is the absence literal for maybe. It is not `nil`: nothing is
"no result", nil is "no object". Consequently `maybe obj` is forbidden;
obj already has nil for absent references, and a successful nil would
be indistinguishable from nothing at runtime. Nested `maybe maybe` is
also forbidden. `maybe prop` waits for the prop design.

## Spelling

    func find(sub is str, s is str) returns maybe int
        ...
        fail                    // or: return nothing (same thing)
        ...
        return i                // success
    endfunc

    func take(from is int) can fail
        if counter == 0
            fail                // nothing left to take
        endif
        counter = counter - 1
    endfunc                     // falling off the end is success

- A value-returning fallible func declares it in the type: `returns
  maybe int`. No extra marker; the signature is the truth.
- A valueless fallible func is `can fail` ("take can fail" is the modal
  truth; "take fails" would read as always). Its call is a valueless
  **signal**, not a bool value (can-fail.md, R14): it is consumed by an
  inline `on fail` handler (`take(from) on fail recover()`, the
  statement-level twin of `else`, fallible-consumers.md) or by a bare
  call statement (trap or propagate), so failure cannot be silently
  dropped. It may not be captured, negated, combined, or stored, nor
  tested by a plain `if` / `while` (those are bool-only); a `can fail`
  call in a value or condition context is a teaching error. Use `can
  fail` for an action whose failure should *flow*; use `returns bool`
  for a pure query whose boolean answer you branch on, store, or
  combine.
- `fail` exits with failure; only legal inside `returns maybe` / `can
  fail` funcs. Falling off the end of a maybe func yields nothing;
  falling off a can-fail func is success.
- Verbs are never fallible: a send crosses the actor boundary and its
  failure story belongs to the send ABI, not here.
- The checker rejects consumers with nothing to consume (`5 else 0`,
  `if var x = 3`): the forms never become decoration.

Uncaught failure inside a fallible func propagates out as that func's
failure (`return xs[i]` in a maybe func fails when the index does);
inside a plain func or verb it traps, and the func must consume it.

## Representation: the null word

A maybe value at rest is one word that is zero for nothing:

- `maybe str` / `maybe list`: the pointer itself; real descriptors are
  never at address zero, so these cost nothing.
- `maybe int` / `maybe bool` / `maybe decimal`: a pointer to a boxed
  word, allocated only on capture (`__exc_box`, arena). Expression flow
  never boxes. `maybe float` is deferred (8-byte box) in v1.

This is also the call ABI: a `returns maybe T` func returns the null
word (so `fail` is "return 0"), and a `can fail` func returns 0 for
failure, nonzero for success. No flag global, nothing for coroutine
capture to preserve, and C host helpers participate by returning 0.

## Lowering: eager unwrap, consumer labels

The whole propagation semantics reduce to one dynamically scoped fail
label in the lowerer:

- Every consumer owns a fail label and installs it while lowering its
  operand: `else` jumps there to run the default, `if var` to the
  else-arm, `while var` to the loop exit, capture to a store-nothing,
  `return` in a maybe func to the fail epilogue. The statement boundary
  installs the default label: the function's trap (plain funcs, verbs)
  or the fail epilogue (fallible funcs).
- Every fallible producer checks eagerly and branches to the current
  label, then yields a plain T: an index bounds-checks and loads; a
  fallible call tests the returned word for zero and unboxes; a maybe
  variable or field read tests and unboxes; `nothing` jumps
  unconditionally.

Because producers yield plain values, no other lowering site changes:
send arguments, log, list builtins, and conditions all receive plain
words as they always did. Icon propagation is not a feature bolted onto
operators; it is the absence of any consumer between the producer and
the label. Left-to-right first-failure-aborts falls out of emission
order, and side effects before the failure point have happened, which
is the predictable reading.

Boxing happens in exactly one place: capture (storing a successful
word-sized value into a maybe variable, field, or parameter). Copying a
maybe to a maybe re-boxes through the same unwrap/capture pair.

## Consequences for indexing

`s[i]` and `xs[i]` become ordinary fallible producers: bounds-checked,
branching to the current fail label. Bare out-of-range indexing
therefore traps instead of today's clamp-to-empty (str) or unchecked
read (list), the trap recommended by else-operator.md; `xs[i] else d`
keeps its meaning through the general `else` consumer, which replaces
the special-cased indexing path in the lowerer.

## What lands in v1, and what waits

In: `maybe T` types (str/list/int/bool/decimal), `nothing`, `returns
maybe T`, `can fail`, `fail`, the capture consumer, the general `else`,
`if var`/`elseif var`, `while var`, fallible indexing, propagation out
of fallible funcs, the null-word ABI with `__exc_box`, and `find(sub,
s) returns maybe int` as the first fallible builtin (replacing the
`!= 0` sentinel that `in` compiles to). Added later the same month
(runtime-errors.md): int/decimal divide and modulo are fallible
producers whose failure is the zero divisor, so `total / count else 0`
composes and a bare zero divide is a located DIV_ZERO fault instead of
a SIGFPE; `INT_MIN / -1` is an OVERFLOW fault, not a failure. Two
clarifications settled there: inference never captures (`var x = a /
b` infers int and traps or propagates; capture stays the explicit `is
maybe T`), and an unconsumed failure's trap reports the producer by
name through a per-site descriptor.

Waiting: `for in` over sources (needs the source type; the pump is just
a fallible call when it arrives), `maybe float`, `maybe prop`, maybe
fields with non-nothing defaults, and fallible sends.


## Decisions (confirmed 2026-07)

1. Full synthesis: maybe T as storable capture over Icon-style
   expression fallibility. Failure propagates in expressions; storage
   is explicit opt-in; plain boundaries trap.
2. Icon propagation: the whole expression fails, left to right, first
   failure aborts, nearest consumer catches.
3. Spelling: `returns maybe T` for value funcs, `can fail` for
   valueless ones, the `fail` statement, `nothing` as the absence
   literal.
4. Settled during ABI design: the null-word protocol (no flag global),
   eager-unwrap with consumer labels, `maybe obj` forbidden (nil is
   that), and uncaught failure propagating out of fallible funcs
   rather than trapping.
