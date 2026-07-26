# The composite data model: records, objects, and `any`

Status: decided (2026-07), not yet implemented. The second pass of the
data-model spine (backlog.md), answering TODO line 48, "unify records, props, and objects to a
form that fits verbs and go-methods", and resolving the flagged `prop`
word-collision. Tier: the record-versus-object split spans World (a builder
meets objects in the tutorial) and Mechanics (records, records.md); the
dynamic `any` value and mixed collections are Mechanics (tiers.md).

Now that records have landed (records.md), the three things the question names
can be placed against each other. The short answer is that there is no
three-way unification to make: there are **two composite poles**, records and
objects, and "prop" was never a third kind. It was a single overloaded word
straddling two unrelated roles, and this pass separates them, retiring the
word.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

## What exists, and the tangle

- **Records** (records.md): plain data, value semantics, named fields,
  behavior by UFCS, no identity, no dispatch. The data pole.
- **Objects** (classes): actors with identity, encapsulated fields, verbs,
  dispatch, and inheritance, reached by a send. The reference pole.
- **`prop`** (`T_TPROP`, `ty_prop`): a surface type the checker calls "the one
  dynamic point: assignable to and from any type, a compiler-level tagged
  union". This is exactly TODO line 30's boxed `{ tag, payload }` dynamic
  value, the intended element type of a mixed `list<prop>`.
- **`any`** (`ET_ANY`, printed "any"): an internal leniency type for values
  the resolver left external, "compatible with everything", which "narrows
  when those layers are modeled". Symbolic data literals get this today
  ("the prop world will type it for real", typed-data.md).
- **`obj`**: the dynamic object handle, "the dynamic escape" (verbs.md) for an
  actor of unknown class.

Two problems fall out. First, `prop` and `any` are the same idea wearing two
names: both are "compatible with everything", one surface, one internal, which
is one dynamic type too many. Second, `prop` is claimed twice: it is the
dynamic tagged value here and in TODO line 30, but shared-params.md reserved
it for a **storable reference** (a deferred pointer). That is the collision
the backlog flagged, and the word cannot mean both.

## The design

### Two composite poles, not three

Records and objects are the whole composite data model, and they are the two
answers to one question, does this thing have identity:

- a **record** is a **value**: its fields are the whole of it, it copies on
  assignment, it has no identity. Data you hold and copy.
- an **object** is an **actor**: it has identity and encapsulated state behind
  a reference, and it carries verbs. An actor you send messages to.

There is no third composite kind between them. "Props" felt like a third
because the word was doing two jobs that are not composites at all, a dynamic
value and a reference, addressed next. This is the C# struct-versus-class
duality (records.md's teaching split), and it is what "fits verbs and
go-methods": see the dot, below.

### The dynamic value is `any`; `prop` retires

The dynamic tagged value, `prop`'s real meaning in the code, **becomes
`any`**, and the word `prop` retires. `any` is a boxed, tagged,
self-describing value, a compiler-level tagged union, assignable to and from
any type and narrowed to a concrete type by `match` or `is` before use. This
single word replaces both `prop` (the surface dynamic type) and the internal
`ET_ANY` leniency, which were the same "compatible with everything" concept
split across a surface and an internal name.

`any` is chosen over `prop` for the reason the whole series weighs names by:
`prop` reads as "property", which a dynamic value is not, and a beginner
cannot decode it, while `any` says exactly what it is, a value that may be of
any type. It is also the mainstream word (TypeScript `any`, Go `any`), so it
is searchable. The `prop` keyword retires with a migration hint pointing at
`any`.

The downstream `prop` references fold into `any`: a mixed collection is
`list<any>` (the machinery is the mixed-list pass, backlog.md, which this
unblocks by settling the name and semantics), `maybe prop` becomes moot since
`any` already spans `nothing`/`nil` (fallible.md), and the `is_ref` wildcard
treatment (nil-nothing.md) applies to `any`.

### The storable-reference reservation is withdrawn

shared-params.md reserved `prop` for a future storable, first-class reference.
That reservation is **withdrawn**: `prop` is the dynamic value (now `any`), so
it cannot also be a reference, and "prop" reads as "property", a poor name for
a pointer besides. A storable first-class reference, if it is ever pursued,
takes a word that names a reference (`ref` is the natural candidate), not
`prop`. The common by-reference need is already met by `shared` parameters
(shared-params.md, call-scoped and unstorable), so nothing waits on this. With
both roles rehomed, the word `prop` disappears from the language entirely,
which is the clean resolution of the collision.

### The dot is the one surface: how verbs and go-methods unify

"Fits verbs and go-methods" is answered by the dot being the single access and
call surface across all of the above, with a fixed resolution order
(verbs.md decision 4):

- `x.field` is a data read or write (records and objects both have fields).
- `x.verb(args)` is a **dispatched send**, and only an object (or `obj`) has
  verbs. This is the actor boundary, late-bound.
- `x.func(args)` is a **UFCS call**, `func(x, args)`, on any value whose first
  parameter matches, records included. This is the go-method, early-bound.

Resolution is field, then verb (objects only), then UFCS func, so a name
resolves to exactly one of them and the reader is never in doubt. So verbs
attach to actors and go-methods (UFCS funcs) attach to values, and the dot
unifies the two into one call syntax without merging their semantics. A verb
dispatches and crosses the actor boundary; a func is a direct call. This is
Pony's `be`/`fun` split and Go's receiver methods, under one dot.

`obj` and `any` are the two dynamic escapes, and they differ in kind: `obj` is
**some actor** (a reference you may send verbs to, dynamically dispatched),
while `any` is **some value** (which may be an int, a record, an obj, ...). An
`obj` is one shape of `any`. To do anything with an `any` you narrow it first,
with `match x` over type-name labels or the `is` type-test, at which point it
is a record you UFCS or an actor you send to.

## Survey

- **C# `struct` vs `class`, plus `object` / `dynamic`**: the value-versus-
  reference split is the record-versus-object model; `object`/`dynamic` are
  the `any` escape. The direct precedent.
- **Go**: value structs plus `any` (the alias for `interface{}`) as the
  dynamic value, receiver methods as go-methods. Excelsior's `any` takes Go's
  name, and the UFCS is Go's receiver model.
- **TypeScript `any` / `unknown`**: `any` is the mainstream name for the
  dynamic type; `unknown` is its must-narrow-first cousin, which is exactly
  the `match`/`is` discipline here.
- **Swift value vs reference types, `Any`**: struct/class value-vs-reference
  plus a capital-`Any` dynamic box. Same shape.
- **Pony `be` vs `fun`**: the async-send-versus-sync-call split that is
  verb-versus-func, the reason the dot can carry both.

Excelsior's stance: two composite poles (value records, reference-actor
objects), one dynamic value type named `any` (retiring `prop`), `obj` as the
dynamic actor, and the dot as the single surface over fields, verbs, and UFCS
funcs.

## Decisions (confirmed)

The five decisions are confirmed; the implementation (retiring the `prop`
keyword with its migration hint, merging `ty_prop` into `any`, and the
`list<any>` / narrowing work) rides the mixed-list pass and the general
`prop`-to-`any` cleanup.


**D1. Two composite poles, not three: records (value) and objects (reference
actors).** There is no third composite kind; "props" was not one. This is the
whole composite data model.

**D2. The dynamic value type is `any`; the `prop` keyword retires.** `any` is
a boxed, tagged, self-describing value (a compiler-level tagged union),
assignable to and from any type and narrowed by `match`/`is`. It unifies the
surface dynamic type (formerly `prop`) with the internal `ET_ANY` leniency.
`any` is clearer than `prop` (which reads as "property") and is the mainstream
name; `prop` retires with a migration hint.

**D3. A mixed collection is `list<any>`; dynamic dispatch is `match`/`is`.**
`list<int>` stays homogeneous and untagged; a heterogeneous list is
`list<any>` of boxed elements. Type names are `match` labels over an `any`
subject and `is` is the single-type test; narrow before you call. The boxing
machinery is the mixed-list pass, which this unblocks.

**D4. The storable-reference reservation on `prop` is withdrawn; `prop`
leaves the language.** A storable first-class reference, if ever pursued,
names a reference (`ref`), not `prop`; `shared` meets the common by-ref need
now. Both of `prop`'s former roles are rehomed, so the word is gone.

**D5. The dot is the single surface; verbs bind to actors, go-methods to
values.** `x.field` (data), `x.verb()` (dispatched send, objects/`obj` only),
`x.func()` (UFCS, any value) resolve in that fixed order (verbs.md decision 4).
`obj` is the dynamic actor, `any` the dynamic value (an `obj` is one shape of
`any`); an `any` is narrowed by `match`/`is` before it is sent to or called.
