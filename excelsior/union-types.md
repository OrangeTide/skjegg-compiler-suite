# Closed unions: `any of (int, str, decimal)`

Status: decided (2026-07), not yet implemented. A revision of the dynamic-value decision in data-model.md
(D2/D3) and mixed-lists.md (D1/D2/D5), prompted by the question "does the open
`any` need to exist at all, or can records and closed sets fill its role?".
Tier: Mechanics (tiers.md); a heterogeneous value that must be narrowed before
use is a systems tool, not tutorial vocabulary.

data-model.md retired `prop` into a single dynamic value type `any`,
"compatible with everything", narrowed by `match`/`is` before use. This pass
sharpens that: on the surface, a heterogeneous value is never *unbounded*. It
is a **closed union of named types**, `any of (int, str, decimal)`, whose
possibilities the reader sees in full and the checker can match exhaustively.
The fully-open `any` stays, but only as the checker's internal leniency type
(`ET_ANY`), never a type a builder writes.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

## Why closed, not open

An open `any` is a hole in the reader's model: it says "some value, could be
anything", and every use must guard against every possibility with no help
from the compiler that the guards are complete. A closed union states the
whole space up front. `any of (int, str, decimal)` reads as "one of these
three", the reader loads exactly three cases, and a `match` over it is checked
for covering all three. This is the same move enums.md made for labels and
typed-data.md made for node kinds: a bounded, named, exhaustively-checkable set
beats an open escape. The three are one family:

- an **enum** is a closed set of bare labels, no payload (enums.md);
- a **union** is a closed set of existing types, each member carrying a value
  of that type (this note);
- a **shape** is a closed set of named node kinds with typed slots, a
  structured tree (typed-data.md).

The dynamic-actor escape `obj` is deliberately *not* closed, and that contrast
is the point: an actor dispatches late by design (the actor model resolves a
verb at send time), so `obj` is legitimately open. A data value has no such
late binding, so there is no reason to leave its type open. Closed for data,
open for actors.

## The design

### The surface type is `any of (T1, T2, ...)`

A closed union is written `any of (T1, T2, ...)`, a parenthesized comma list of
two or more member types:

    var example is any of (int, float, str)
    var cell    is any of (int, str, decimal) = 0
    field slot  is any of (Point, Circle)

The parentheses are required, not optional. They make the member list a
bracketed comma list, which is exactly the quotation boundary's rule (a comma
list runs to a bracket or a keyword, never to a newline, sequences.md), so the
list ends at `)` with no ambiguity against the `=` or the newline that follows.
The small word `of` is revived for this: `any of (...)` reads aloud as the
thing it is, and the whole series prefers a searchable word over a symbol.

Constraints, each a teaching error:

- **At least two members.** `any of (int)` is just `int`; the error says so.
- **No duplicate members.** `any of (int, int)` names the repeat.
- **No absence words.** `nothing` and `nil` may not be union members; the error
  points at `maybe T`, which is the absence spelling (fallible.md, nil-nothing.md).
  `any of (int, nothing)` is `maybe int`; `maybe obj` stays forbidden.

Members may be scalars, records, or classes. A union of record types is a
proper tagged variant (`any of (Point, Circle)`); a union of classes is a
closed actor set, narrower than `obj`. The absence axis stays separate from the
union axis, so a union is always over present, concrete types.

### It is anonymous and structural

An inline `any of (...)` has no declared name and is identified by its member
*set*, order-insensitive: `any of (int, str)` and `any of (str, int)` are the
same type. This is the opposite of enums, which are nominal and ordered because
a label has declaration identity. A union has no labels of its own, only member
types, so two unions with the same members are the same union. Naming a union
(a `type Value = any of (...)` alias) is a later convenience; the core is the
anonymous inline form the examples show.

### Representation: the bounded box from mixed-lists

A union value is the boxed `{ tag, payload }` of data-model.md and
mixed-lists.md, with the tag space **bounded to the union's member set**: the
tag is a small int indexing the members, not an open type id. So the whole
box/`list<any>` machinery carries over unchanged, only the tag universe shrinks
to a per-union closed set. A constant union value folds its box into the
constant global; a runtime one boxes through the existing box helper
(fallible.md). Because the box is pointer-sized, a `list<any of (...)>` reuses
the word-sized list machinery exactly as mixed-lists.md's `list<any>` did, and
a union can still carry an oversized payload (a float, a record) by pointer.

### Use is by narrowing, and the `match` is exhaustive

A union value cannot be operated on until its member is known, the same
narrowing discipline as before:

    match cell
        int     then total = total + cell     // cell is int here
        str     then say(cell)                 // cell is str here
        decimal then money = money + cell
    endmatch

`match` arms are the member type names, each narrowing the subject to that type
in its arm, lowering to tag compares. The new strength over an open `any` is
**exhaustiveness**: a `match` over a union must cover every member or supply an
`otherwise`, else a compile error naming the missing members, exactly as an
enum `match` is checked (enums.md D4). The reader and the compiler agree on the
full case set. `x is int` remains the single-type test yielding a bool, the
one-off form when a full `match` is more than needed.

### What it does not replace

A union is "one of these whole types". It is not a schema for structured trees:
typed-data.md's shapes keep their named slots, nested kinds, and `to`/label
resolution, which a union has no vocabulary for. Use a union for a small closed
set of alternative value types (a cell that is an int or a string), and a shape
for a tree of typed nodes (a dialog graph). A union of record types is the
lightweight tagged variant; a shape is the structured document.

## What changes in the settled notes

- **data-model.md D2** said the surface dynamic type is an unbounded `any`.
  Revised: the surface has no unbounded `any`; the dynamic value is a closed
  union `any of (...)`. The `prop` retirement stands. `ET_ANY` stays, now purely
  internal (checker leniency for not-yet-modeled and open quoted values), never
  builder-written.
- **data-model.md D3** and **mixed-lists.md D1/D2/D5** used `list<any>`.
  Revised to `list<any of (T1, T2, ...)>`: the mixed list declares its member
  set, and the tag universe is that set rather than the open ~8-tag list. All
  other mixed-list decisions (declared-not-inferred, construction-time boxing,
  `match`/`is` narrowing) carry over, now with exhaustiveness on the `match`.
- **`obj`** is unchanged: the open dynamic actor, closed only at the actor
  boundary by dispatch, is the one place open-endedness is intended.

## Survey

- **Rust `enum` with payloads, Swift `enum` with associated values**: the
  closed tagged union matched exhaustively. The direct model; Excelsior's
  `any of (...)` is the anonymous, structural version.
- **TypeScript union types `int | string`**: anonymous structural unions
  narrowed by tag checks. The same shape; Excelsior spells the bar as the
  searchable `any of (...)` and requires the `match` to be exhaustive.
- **F# / OCaml / Haskell sum types**: named, exhaustively-matched variants, the
  functional ancestor. Excelsior keeps the exhaustiveness, drops the per-member
  constructor names for the anonymous member-type form.
- **C `union` + tag, Pascal variant records**: the untagged/hand-tagged
  ancestor a bounded box makes safe.
- **Go `any` / TypeScript `any`**: the *open* dynamic type this pass declines
  to put on the surface, keeping it internal (`ET_ANY`) so every heterogeneous
  value a builder writes is a closed, matchable set.

Excelsior's stance: TypeScript's structural unions with the exhaustiveness of a
Rust/Swift `enum`, spelled `any of (...)` for searchability, over the boxed
`{tag, payload}` from mixed-lists with the tag bounded to the member set, and
no unbounded `any` on the surface at all.

## Decisions (confirmed)

The seven decisions are confirmed. The implementation (the `any of (...)` type
syntax and its member-set constraints, the bounded-tag box shared with the
mixed-list work, the exhaustive type-name `match`, and the `prop`-to-internal-
`ET_ANY` cleanup) rides the data-model spine, and depends on records
(records.md) for record members and on mixed-lists.md for the box machinery.

**D1. The surface heterogeneous type is a closed union `any of (T1, T2, ...)`,
never an unbounded `any`.** The reader sees the full space and the compiler
matches it exhaustively. `ET_ANY` remains, internal only.

**D2. The syntax is `any of (...)`, a parenthesized comma list of two or more
distinct member types.** Parentheses are required (the bracketed-comma-list
boundary). At least two members, no duplicates, no `nothing`/`nil` members
(the error points at `maybe T`). Members may be scalars, records, or classes.

**D3. A union is anonymous and structural, identified by its member set,
order-insensitive.** `any of (int, str)` equals `any of (str, int)`. This
contrasts with the nominal, ordered enum. A named `type` alias is a deferred
convenience.

**D4. The representation is the bounded box.** The `{tag, payload}` box of
data-model.md / mixed-lists.md, with the tag space bounded to the member set.
`list<any of (...)>` reuses the word-sized list machinery; constant boxes fold.

**D5. Use is by narrowing, and the `match` is exhaustive.** `match` arms are
member type names narrowing per arm; the match must cover every member or
supply `otherwise`, else a compile error names the missing members (enums.md
D4). `is` is the single-type test.

**D6. A union does not replace shapes.** Shapes keep structured trees with
typed slots and `to`/label resolution (typed-data.md); a union is a small
closed set of alternative whole types. A union of record types is the
lightweight tagged variant.

**D7. This revises data-model.md D2/D3 and mixed-lists.md D1/D2/D5** to the
closed-union form, and leaves `obj` as the one intentionally open dynamic type
(closed at the actor boundary by dispatch).
