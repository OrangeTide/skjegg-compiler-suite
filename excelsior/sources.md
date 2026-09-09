# Sources: the fallible pump as the iteration protocol

Status: decided (2026-07); implemented, both halves. The cursor half is
D2/D3 over records (`tests/exs_source_cursor.exs`); the continuation
half (`source of T`, `yield`, the second-class checks,
`tests/exs_source_yield.exs`) landed with one mechanism amendment,
recorded at D1: the backing is a stackful coroutine on a private
per-source stack, not a reuse of `__cont_capture`/`__cont_resume`,
whose restored segments are tied to the first pump's stack region and
corrupt the stack when a source is pumped from a deeper frame.
Extracted from
else-operator.md's "Sources" exploration and settled in its own pass.
Both backings are in the design: the cursor record (plain data, nearly
free on today's machinery) and the failable continuation (`source of T`,
reusing the delimited-continuation machinery TinC and TinScheme already
lower). Lists, strings, and ranges keep their compiler-inlined counted
loops and never involve a source value. Tier: Mechanics (a builder
writes `for x in xs` and never sees a source; declaring one is a systems
author's job); tiers.md.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

## The idea

Icon's `every i := find("or", sentence) do write(i)` raised whether the
language needs generators. The answer that survived discussion: the
fallible pump IS the iteration protocol, and everything else follows.

`while var i = next(c)` already works today (fallible.md's consumers),
so "iterable" has a definition: anything that can be pumped for a
value-or-failure. Then `for i in expr` is sugar for that loop:

    for i in positions("or", sentence)     // any source works here
        write(i)
    endfor

    var c = positions("or", sentence)      // or pump it by hand
    while var i = next(c)
        write(i)
    endwhile

(`find(sub, s)` stays the one-shot first-position lookup, `returns
maybe int`; a successive-positions producer is a source a library
declares, `positions` above.)

## Decisions

**D1. One pump face, two backings.** A source is anything `next` can
pump: `next(c)` is a fallible producer, `while var x = next(c)` the
manual loop, `for x in c` its sugar. Behind the face:

- **A cursor record**: a record holding explicit position state, with a
  `next` func (D2). Plain data, no new type, no restrictions; the state
  is declared and visible. This half needs almost nothing new, only the
  `for` desugar (D3).
- **A failable continuation**, typed `source of T` (D4): a suspended
  computation each pump resumes until it fails. This is Icon's generator
  minus the two poisons: the state is a declared first-class value
  rather than invisible call-site state, and resumption happens only at
  the pump. The pump is the delimiter, the turn the outermost one.

  > Mechanism amendment (implemented 2026-07): the pass had named TinC
  > and TinScheme's `__cont_capture`/`__cont_resume` as the machinery,
  > but those copy a stack segment and restore it to its original
  > absolute addresses, which is safe only when resumption happens
  > under the still-live delimiter frame (the reset/shift discipline).
  > A source is created at one depth and pumped from others; a pump
  > deeper than the first would restore the body's segment over live
  > frames. The safe form of the same delimited idea is a **stackful
  > coroutine**: each source carries a small private stack (4KB, from
  > the arena, no overflow check yet; the quota fault is memory.md's),
  > the body's frames live at fixed private addresses, and
  > `__exc_src_next`/`__exc_src_yield` (runtime/start.S) switch
  > contexts with no copying, so a pump is safe from any caller depth.
  > Surface semantics are unchanged.

**D2. The pump is the `next` convention, not a keyword.** For a cursor
record `C` the pump is an ordinary fallible func the author writes:

    func next(shared c is C) returns maybe int

`shared` because the pump advances the cursor in place (a by-value
record parameter is a copy and could not); the func resolves by the
normal rules, and UFCS spells it `c.next()`. No marker declares a
record a source: a record is iterable exactly when a `next` for it is
in scope, the structural stance object-slices.md already takes for
conformance. For a `source of T` value, `next` is the built-in pump
(same spelling; the checker routes on the operand's type, the way `+`
routes between int, str, and list). Scoping consequence, recorded
honestly: funcs are class members, so a v1 cursor source is usable
where its `next` is visible, the declaring class; a cross-class library
source arrives with capabilities.md `include` carrying the record and
its funcs together.

**D3. `for x in E` desugars by E's type.** A `list`, `str`, or `a to b`
range keeps today's inlined counted loop (no source value, no change).
A record with a resolvable `next` becomes

    var __c = E
    while var x = next(__c) do BODY endwhile

with `break`/`continue` belonging to the loop as usual. A `source of T`
takes the same desugar through the built-in pump. Any other type is a
teaching error naming the three iterable shapes. Comprehensions
(function-values.md) lower through `for`, so they iterate any source
with no further rule.

**D4. A continuation source is declared by its return type and produces
with `yield`.**

    func walk(root is obj) returns source of int
        yield 1
        yield 2
    endfunc

Calling `walk(r)` does not run the body; it hands back the suspended
computation. Each pump resumes the body to its next `yield expr`, which
produces that value and suspends; `fail` or falling off the end
exhausts the source (the pump's failure). `return` keeps its one
meaning, "done with this func", and is a compile error in a source body
(the word for producing is `yield`, the word for stopping is `fail`).
`yield` is a new keyword, legal only inside a func returning `source of
T`; it is the cross-language word and plain English ("yield a value"),
where Icon's `suspend` describes the mechanism rather than the intent.
`source of T` follows the `of` convention (type-parameters.md);
`source` and `of` are contextual words like `set of`, so `source`
remains usable as an identifier. T is a word-sized element type, the
same set a list element may be; float waits with float-element lists.

**D5. The second-class rule binds `source of T` only.** A cursor record
is plain data: storable in a field, sendable, comparable like any
record, since a stale position is just data. A `source of T` value is a
suspended stack, so it is second-class in the shared-params.md D7
style: it lives as a local, passes down into funcs as a parameter, and
may be returned (a wrapper like filter or take-while returns a source
wrapping another), but cannot be stored in a field, captured into
persistent state, or sent to another actor, enforced by the checker at
those sites. It is consumed within the turn that made it, so the freeze
boundary never sees a suspended stack (the same stance memory.md D8
takes for closures and buffers). Persist the results as a list; a
persistent producer is a session, which the actor model already
provides.

**D6. Delegation is ordinary code.** A func can take a source parameter
(the concrete cursor record type, or `source of T`), consume part of
it, wrap one source in another, or return one. Icon's `every` becomes a
plain loop. A parameter generic over "any source" is not a base-layer
type; a typed macro (typed-macros.md) dispatches on `typeof` when a
library wants one face over both.

Rejected, precisely: implicit caller-saved call-site state (undeclared,
breaks on nested loops and recursion, which is why Icon ties it to the
generator frame), and suspended control flow that outlives the turn.
The principle: **generation is data, a turn-local declared source, or
an actor; never hidden call-site state, never suspended control flow
across the freeze boundary.**

## Implementation staging

The halves land independently, cursor first:

1. **Cursor half** (implemented 2026-07): the D3 desugar in the
   checker/lowering (records, `shared`, fallible funcs, and `while var`
   all existed). No grammar change; no runtime change. The checker's
   `iter_element` resolves and validates `next` and stamps its `maybe T`
   on the for node; the lowering copies the cursor unless the iterated
   expression is a fresh ctor and pumps through the ordinary
   fallible-call path, `continue` re-pumping.
2. **Continuation half** (implemented 2026-07): the `source of T` type
   and the `yield` keyword; a source func lowers to a constructor (the
   declared symbol, allocating the struct plus private stack via
   `__exc_src_new` and storing its arguments into it) and a suspended
   `__body` function the pump launches on the private stack; `yield`
   boxes through the null-word maybe protocol and switches back via
   `__exc_src_yield`; returning from the body (or `fail`) is
   exhaustion, turned into the pump's 0 by the launch shim; and the D5
   checker rules (no field, no record field, no verb parameter or
   return, no send argument, no `shared` parameter on a source func,
   `return` rejected in a source body). The pump `next` routes by
   argument type, so it coexists with a class's cursor `next` func.

## What already leans on this design

- fallible-consumers.md's consumer set: `while var` is the pump loop
  and `for in` its sugar, the fourth consumer of fallibility.
- The second-class rule is the never-suspended-across-the-freeze-
  boundary stance memory.md D8 takes for closures and buffers, and
  host-abi.md's freeze/thaw assumes.
- function-values.md's `for NAME in SOURCE` binder wording names a
  source; comprehensions iterate one through D3.

## Deferred

- Cross-class library sources (waits on capabilities.md `include`).
- A generic any-source parameter (typed-macros.md dispatch is the
  spelling; no base-layer source interface).
- Float elements (with float-element lists), and float parameters on a
  source func (the constructor stores word-sized argument words; a
  teaching error rejects one).
- A `source of T` field or send: never, by D5 (not deferred, decided
  against).
