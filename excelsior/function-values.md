# Function values: `func` as the primitive, named by binding

Status: decided (2026-07), not yet implemented. The lambda/block pass from the backlog (backlog.md),
the design `list-ops.md` (D5) defers `map`/`filter`/`reduce` and `sort` to. Tier:
comprehensions are World; a `func` value and the combinators are Mechanics; an
escaping captured closure is deferred. Builds on the actor-safety rule of
shared-params.md (call-scoped, never crosses the actor boundary), the de-arrow
decision (no `->`), string-literals.md (`{...}` is a string, not a block), the
meta-layer definition model (meta.md: `class`/`record`/`verb` are prelude
macros), and union-types.md (a multi-type return).

`list-ops.md` deferred the higher-order operations to "a lambda/block design".
Designing it turns up a guiding constraint the whole feedback pass sharpened:
**every construct here should be an ordinary function or a binding, so a library
author can synthesize and extend it through the macro system, not a special form
frozen into the compiler.** That constraint decides the shape: `func` is the one
primitive value, a named function is readable sugar over a uniform binding core
(so a macro can emit one, like `class`/`record`/`verb`), sorting is an ordinary
function taking a `func`, and the only other sugar is a comprehension that lowers
to a loop.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

## Why the naive lambda does not fit

The obvious lambda, a brace-delimited block with an arrow binder, cannot be
written in Excelsior, and should not escape if it could:

- **`{...}` is a string** (string-literals.md), so `{ x -> x * 2 }` reads as a
  string literal. Braces are gone as a block delimiter.
- **`->` is removed** (the de-arrow decision), so a binder is a word.
- **An escaping closure is an isolation hazard.** An object is an actor with
  private state reachable only through sends; a closure that captured a local or
  `self` and then escaped (stored, sent, returned and kept) would carry one
  actor's guts into another's turn and outlive the arena turn it was built in.

So the lambda is word-spelled and, when it captures, non-escaping. Both fall out
of reusing machinery the language already has.

## Two surfaces, one core

The primitive **value** is the anonymous function, **`func(params) returns T ...
endfunc`**. Beneath it is one **core**: binding a name to a func value. Two
readable surface forms both reduce to that core, so neither is a special compiler
form:

- The **named declaration** stays, because it is what an author writes most and
  reads most clearly:

      func countHats(heads is int, depth is int) returns int
          ...
      endfunc

  It is **sugar that expands to binding `countHats` to a func value**, a prelude
  macro over the uniform core exactly as `class` / `record` / `verb` are
  (meta.md). So it maps to the macro system and is synthesizable, which was the
  real requirement, and it is met without deleting the readable form.

- The **anonymous lambda** `func(...) ... endfunc` is the value, passed inline or
  bound to a local name for reuse:

      const compare = func(a is Card, b is Card) returns bool
          return a.rank > b.rank
      endfunc
      sort(hand, compare)

Because the core is a plain binding, a macro that emits a function emits the core
form, and the named declaration is itself one such macro. This also keeps `func`
and `verb` **symmetric**: both are named-member surfaces over the same core
(meta.md), rather than one a declaration and the other a binding.

An earlier revision proposed discarding the named form entirely (every function
written `let name be func(...)`); that was overtuned. The extensibility goal is
served by the *core* being uniform and macro-targetable, not by removing the
surface an author reads, so the readable named declaration is retained. A lambda
is bound to a local name by reusing the existing **`const f = func(...)`**
immutable binding, no new keyword; the load-bearing decision is the single core
that both surfaces and macros share. **Verbs are the parallel case**: a `verb name(...)
... endverb` is the dispatched, public, sendable actor interface, the same
named-member-over-core shape, on the dispatch axis.

## Types are explicit; an untyped func belongs to the macro layer

A `func` value's type is **explicit in the base layer**: parameters are typed and
the result is a written `returns T`. Returning more than one type is fine when it
is **stated** as a union, `returns any of (int, str)` (union-types.md); a
side-effecting func with no result is `func(params)` with no `returns`.

Types may be **inferred only when the whole function type is fixed by context**,
an anonymous literal passed to a combinator whose signature supplies the
parameter and result types:

    total = reduce(xs, 0, func(acc, x) return acc + x endfunc)   // acc, x, result from reduce's type

Here nothing is untyped: `reduce`'s parameter pins the function type, and the
literal restates none of it. But a **bare `func` with no annotations and no
context is not base-layer code**. An implicit result type that reads as "no type
at all" is the signature of the untyped meta layer, not the statically-typed
base, so the base layer requires the type to be present, either written or fixed
by context.

## The combinators are ordinary functions

The higher-order operations are **ordinary functions that take `func` values**,
so a runtime author can define, wrap, and extend them:

    map(xs, f)              // f is func(T) returns U
    filter(xs, keep)        // keep is func(T) returns bool
    reduce(xs, init, step)  // step is func(U, T) returns U
    sort(xs, before)        // before is func(T, T) returns bool  (or a key func)

None is a compiler special form. `sort` in particular takes a comparator or key
**function**, not a baked `by` clause: a `sort(xs) by each.score` construct would
be an ordering algorithm frozen into the grammar, unextendable and
unsynthesizable by a library author, so it is **declined**. Sorting by a field is
a one-line `func`, and a library may offer its own `sortByKey(xs, keyfunc)`
helper over `sort`, which a baked clause could never allow.

## Comprehensions: the one sugar, and it is just a loop

Map and filter are common enough to deserve a readable surface, and a **list
comprehension** provides it without a function value. It is control-flow sugar
that **lowers to a `for` loop building a buffer** (buffer.md), so it is not a
baked algorithm and can be a prelude macro over `for`, not compiler magic:

    doubled = [x * 2 for x in xs]
    adults  = [p for p in people if p.age >= 18]

The binder is **exactly a loop header**, `for NAME in SOURCE`: `for` always pairs
with `in` (one opens the binder, one names its source), the same rule as a `for`
statement, so there is nothing new to learn about when each word appears.

**Multiple captures are multiple `for` clauses**, each nested inside the one
before it, which is what a two-level operation like a grid walk or a rotate
needs:

    pairs = [ [x, y] for x in xs for y in ys ]        // every (x, y), y inner
    cells = [ tile(r, c) for r in rows for c in cols ]

A structural two-dimensional result (a matrix transpose or rotate) is a
**nested comprehension in the head**, the inner comprehension building each row:

    rotated = [ [grid[r][c] for r in rows] for c in cols ]

Parallel iteration (walking two lists in lockstep) is a `zip` **function**, not
new syntax: `[f(x, y) for pair in zip(xs, ys) ...]`, keeping with functions over
special forms. A comprehension is complementary to `map(xs, namedFunc)`: the
comprehension inlines a per-element expression, the combinator reuses a function
defined once.

## Non-escaping: a captured closure is call-scoped, like `shared`

A `func` that **captures** enclosing locals or `self` is **non-escaping /
call-scoped**, the discipline shared-params.md proved safe: it flows only
downward into calls and is never stored in a field, returned, sent across a send,
or held as a list element. Within those bounds it may **read** the captured
locals and `self` fields, safe because it runs **synchronously within the current
turn**, so the state it reads is still live and cannot cross into another actor.
This is Kotlin's inline lambda and Swift's non-escaping closure, the same rule
`shared` already established.

A `func` that **captures nothing** (a top-level or class-level named function) is
a **free value**, referenceable and passable like any other value, because there
is nothing to outlive. The restriction is about capture, not about naming.

The escaping cases, a captured closure stored in a field, returned as a factory
result, or registered as a cross-turn callback, are **deferred to the
memory-management pass** (which settles the lifetime a persisted closure needs).
The actor-safe way to store behavior on an object is what the model already
offers: a **verb** or an included **capability** (capabilities.md), not a closure
in a field.

## What is deferred

- **Escaping / stored captured closures** (a function-typed field, a returned
  closure, a function element in a list), pending the memory-management pass.
- **Named aggregates** (`sum`, `count`, `min`, `max`, `all`, `any`, `join`) over
  a comprehension head, the World-tier reduce-avoidance; the general `reduce`
  covers them at Mechanics tier meanwhile.
- **A concise expression-body form** for a `func` literal (dropping `return` for
  a single expression), additive once the base form is settled.

## Survey

- **Scheme / ML**: `define`/`let` binds a name to a lambda; there is no separate
  function-declaration form, exactly the "a function is a bound value" model this
  adopts.
- **Kotlin**: inline lambdas that do not escape by default; Excelsior takes the
  non-escaping default and requires a real element name over `it`.
- **Swift**: `@noescape` (the default) versus `@escaping`, the distinction drawn
  here, with escaping deferred to the lifetime model.
- **Python / Haskell comprehensions**: `[f(x) for x in xs if p(x)]` with multiple
  `for` clauses nesting, adopted wholesale because every keyword is a word and it
  lowers to a loop.
- **Ruby `sort_by` / SQL `ORDER BY`**: sorting by a key; taken as a *function*
  argument here rather than a syntactic clause, so it stays extensible.
- **AppleScript / Inform 7**: the audience's tools have no inline lambda, the
  evidence that a general `func` value belongs at Mechanics tier.

Excelsior's stance: one primitive `func`, named by an ordinary `let` binding so
definitions are macro-synthesizable; combinators (including `sort`) are ordinary
functions taking `func` values, never baked clauses; comprehensions are the one
sugar and are just a loop; a captured `func` is non-escaping like `shared`, with
escaping closures deferred.

## Decisions (confirmed)

The seven decisions are confirmed. The implementation (the `func` value and its
type, the named-declaration prelude macro over the binding core, the
`const f = func(...)` local binding, contextual type inference for literals, the
non-escaping capture check, the comprehension-to-loop lowering, and the
combinators over func values) follows; escaping captured closures wait on the
memory-management pass, and the named aggregates are a follow-on.

**D1. `func(params) returns T ... endfunc` is the anonymous function-value
primitive.** No `{}` block (braces are the string delimiter) and no `->` binder
(de-arrowed); a valueless func omits `returns`.

**D2. Two readable surfaces over one core.** The named declaration `func
name(params) ... endfunc` is retained (the everyday, most readable form) and the
anonymous lambda `func(...) ... endfunc` is the value; both reduce to one core,
binding a name to a func value. The named declaration is a prelude macro over
that core (like `class`/`record`/`verb`, meta.md), so it maps to the macro system
and is synthesizable without being deleted, which was the real requirement.
Discarding the named form was overtuned: uniformity comes from the shared core,
not from removing the surface. A lambda is bound to a local name by reusing the
existing `const f = func(...)` immutable binding, no new keyword. `func` and
`verb` are the symmetric named-member surfaces over this core, on the value and
dispatch axes.

**D3. A function's parameter and result types are explicit in the base layer**,
`func(a is T1, b is T2) returns U`, a multi-type result stated as a union
(`returns any of (...)`). Types may be inferred only when the whole function type
is fixed by context (a literal passed to a combinator); a bare annotation-free
`func` with no context is macro-layer (untyped), not base-layer, code.

**D4. A captured `func` is non-escaping / call-scoped (the shared-params.md
rule):** downward into calls only, never stored in a field, returned, sent, or a
list element; a read-only closure over live locals and `self`, safe because
synchronous within the turn. A `func` that captures nothing is a free value.
Escaping captured closures are deferred to the memory-management pass; the
actor-safe substitute is a verb or capability.

**D5. Map and filter are list comprehensions, control-flow sugar that lowers to a
`for` loop building a buffer** (a prelude macro, not a baked algorithm):
`[EXPR for x in xs (if COND)?]`. The binder is a loop header (`for` always pairs
with `in`); multiple captures are multiple `for` clauses (nested), a
two-dimensional result is a nested comprehension in the head, and parallel
iteration is the `zip` function. Complementary to `map(xs, namedFunc)`.

**D6. sort-by is declined; sorting is an ordinary function `sort(xs, cmp)` taking
a `func` value** (a comparator or key), extensible and definable by a library or
runtime author and lowering to no special form. A baked `by` clause would freeze
an ordering algorithm into the grammar, unextendable and unsynthesizable, against
the guiding constraint of this pass.

**D7. The combinators `map` / `filter` / `reduce` / `sort` are ordinary functions
taking `func` values (Mechanics tier).** Named aggregates (`sum`/`count`/...) as
the World-tier reduce-avoidance are a deferred follow-on; the general `reduce`
covers them meanwhile.
