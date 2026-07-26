# Records: plain data with value semantics

Status: decided (2026-07); implemented (`tests/exs_record.exs`): construction
both positional `Point(3, 4)` and named `Point(x: 3, y: 4)` (a record literal;
the separator settled as `:`, order-independent, with field defaults). The same
`name: value` form is a **named function/verb argument** (`mix(add: 10, base: 5)`,
reordered to parameter order), guarded by the safety rule that a parameter or
field without a default must be named. Field read/write `p.x`,
value-copy semantics on assignment (a record is a one-word pointer to an arena
block, copied on bind; the inline layout of D6 is a deferred optimization), and
**UFCS** `p.norm()` resolving to `norm(p)` (D4: field-first, then a func in the
class whose first parameter receives the record; `tests/exs_record_ufcs.exs`),
and **value equality** (D5): `a = b` is true when all fields are equal, so
distinct copies with equal values compare equal (`tests/exs_record_eq.exs`;
scalar and str fields, a list/nested-record field guarded out for now). Deferred: records through sends (D5), `list<Point>`, nested records, and
compile-time introspection (D8). The
foundational pass of the data-model spine (backlog.md). Tier: Mechanics (tier 2); a record is a structuring tool for the
systems author, named in the tutorial and taught in the mechanics guide
(tiers.md). Records are the prerequisite several later passes name ("when
records land"): the records/props/objects unification, mixed `list<prop>`,
record introspection, and verbs.md's UFCS for records.

A `record ... endrecord` declaration already parses and resolves (SYM_RECORD
with a member scope), but a record name types as "a type, not a value" and
there is no record value: no construction, no field access, no lowering.
Records are declared-but-dead-ending, exactly where enums were before
enums.md. This note settles what a record value is so the construct becomes
first-class.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

## What a record is

**A record is plain data: a fixed set of named, typed fields, with value
semantics.** It has no identity, no verbs, no dispatch, and no inheritance. It
is copied when assigned and when passed by value. A record is the data pole
opposite the class/actor pole:

- an **object** (a class instance) is an **actor**: it has identity and
  encapsulated state, it carries verbs, it is reached by a send, and a
  variable holds a reference to it. Two variables naming the same object name
  the same actor.
- a **record** is a **value**: its fields are the whole of it, assigning it
  copies it, and there is no identity behind the fields. Two variables holding
  equal records hold two independent copies.

The one-sentence teaching split: **a record is data you hold and copy; an
object is an actor you send messages to.** This is the C# struct-versus-class
line, chosen because it maps onto the actor model the language already has
rather than adding a second object system.

    record Point
        x is decimal = 0
        y is decimal = 0
    endrecord

    record StatBlock
        str is int
        dex is int
        hp  is int = 10
    endrecord

Record fields are all public: a record's fields are its whole value, so it
has no `public` / `private` sections (visibility.md's sections are an
actor-encapsulation concern; a record has nothing to encapsulate). Fields may
carry defaults (`hp is int = 10`), which construction can rely on.

## Value semantics

Assigning a record copies it; passing it to a func copies it; returning one
hands back a copy. Mutating a copy never touches the original:

    var a is Point = Point(x: 1, y: 2)
    var b is Point = a        // b is an independent copy
    b.x = 99                  // a.x is still 1

This is the intuitive "a copy is a copy" behavior, and it is what makes a
record the right vehicle for **returning several outputs** (shared-params.md):
a returned record is the caller's own copy, with no shared state to reason
about. To let a helper mutate a caller's record in place, pass it to a
`shared` parameter (shared-params.md); that is the single, explicit opt-in to
by-reference, and it is why records do not need reference semantics of their
own.

The copy is shallow, and the existing value model makes that safe: a `str`
field copies a pointer to an immutable string (sharing is invisible), a `list`
field copies a pointer to a copy-on-write list (a later mutation forks), and
an `obj` field copies a handle, which correctly shares the actor (the actor is
a reference type; the record holds a reference to it, like a Go struct with a
pointer field). So copying a record is copying its word-sized fields, with no
deep walk and no new lifetime rule.

## Memory: inline, fixed size, no heap identity

A record has a fixed layout known at declaration, so it lives **inline** in
its container: a local slot, a field of an object or another record, or an
element of a `list<Point>`. There is no separate heap object and no identity,
so a record needs no garbage collection of its own; it is reclaimed with its
container. This fits the arena model and Compact Pascal's value records
directly. A record that must refer to itself (a tree node pointing at another
node) cannot do so with a direct record field, since value semantics would
make it infinite; the indirection is an `obj` reference or a list, the same
rule Pascal has.

## Construction: the type name applied

**A record value is constructed by applying its type name, `Point(...)`.**
This parallels `spawn(Class)` for actors and marks the distinction: you *make*
a record value directly, because it is just data, and you *spawn* an actor,
because it has a lifecycle and identity. It is a word, not a delimiter (the
backlog's set-constructor lesson), so there is no `[...]` overload to
disambiguate and nothing new to search for.

Fields may be given by name, which is the clear and default-friendly form:

    var origin is Point   = Point(x: 0, y: 0)
    var hero  is StatBlock = StatBlock(str: 12, dex: 9)   // hp defaults to 10

Named construction is order-independent, self-documenting, and lets a field
with a default be omitted. A positional form `Point(3, 4)` is allowed for a
short record where the field order is obvious. The named-field separator (a
leading candidate is `:`, as in `Point(x: 3)`; `=` is the alternative) is a
surface sub-decision to settle in implementation, since `:` interacts with the
retired colon-send migration hint.

## Field access and behavior

Field access is `p.field`, for read and, on a mutable record lvalue, write:

    var p is Point = Point(x: 1, y: 2)
    p.x = p.x + 5          // read and write a field

Records carry no funcs of their own; **behavior attaches by UFCS**, activating
verbs.md's decision 4. A free func whose first parameter matches the record is
called in receiver position:

    func norm(p is Point) returns decimal
        return sqrt(p.x * p.x + p.y * p.y)
    endfunc

    var d is decimal = p.norm()        // resolves as norm(p)

The dot resolution order is fixed and documented (verbs.md): a member field
first, then a member verb (classes only, never records), then a UFCS func. So
`p.x` is always the field and `p.norm()` is the func call; a record never
dispatches. This is Go's and Compact Pascal's receiver-method model, with zero
new grammar and no methods living on the record.

## Records in the type system

A record is a **nominal** type: a `Point` is a `Point`, distinct from any
other record with the same fields. Nominal typing is the safe default;
structural conformance is the separate, deferred interface-types work
(verbs.md decision 5), not a property of records. A record type is usable
wherever a type is:

- as a **field type** of an object or another record (nesting, stored
  inline);
- as a **local, parameter, or return type**, including the multiple-return
  vehicle above;
- as a **list element type**, `list<Point>` (records are word-addressable
  inline elements; unlike a float element, a record's fixed layout is fine).

**Equality is by value**: two records are equal when all their fields are
equal, recursively. There is no identity to compare, which is the point.

**A record crosses a send by copy.** Because a record is data with no
identity, it can be a verb argument or return value, marshaled as its fields
across the actor boundary, where an object handle would instead share an
actor. This is what lets a record carry structured data between actors and out
of a verb, and it is the same copy the value semantics already promise.

## Introspection for macros

Composite types must be **introspectable at compile time** so macros can be
built around them. A macro that generates a serializer, a pretty-printer, or a
pattern-match over a record's fields needs to ask the record about its own
shape, so records are designed to expose their structure to compile-time code
from the start rather than have it retrofitted.

The minimal primitives, smallest useful first:

- **field presence** — a boolean "does this record type have a field named
  N", the check to reach for first (Zig `@hasField`, D `__traits(hasMember,
  T, "n")`).
- **field type** — "is field N of type T", or "what is the type of field N",
  so a macro can branch on a field's type (Zig `@FieldType`).
- **field iteration** — walk a record's fields as (name, type) pairs, the
  primitive a serialize or map-over-fields macro is built from (Zig
  `@typeInfo` + `inline for`, Nim `fieldPairs`, D `__traits(allMembers)`).

Two properties matter. It is **compile-time**: a macro consumes the record's
structure at expansion, and the introspection compiles away to nothing, so
there is zero runtime cost and no runtime metadata (the Zig / D / Nim model,
not Go's runtime `reflect`). And it is **Meta-tier** (tiers.md): a builder
never writes it, a macro or IMMEDIATE word does, over the Mechanics-tier
record.

This note records the requirement and the minimal primitive shape so records
are designed to support it; the exact spelling, and how it threads through the
compile-time macro surface, is the record-introspection pass (backlog.md),
which needs the meta layer's surface and so waits on that layer's design. What
this note fixes is the precondition for all of it: a record's fields are a
first-class, named, ordered set the compiler can be asked about.

## Records are not shapes

Records do not replace or merge with shapes (typed-data.md). A **shape**
validates symbolic `[...]` data literals for the story writer, who reads the
literal, not a constructor; a **record** is a nominal typed struct the
mechanics author constructs and mutates. They serve different personas and
different surfaces. typed-data.md already recorded that if the record/variant
machinery landed first, a shape could become sugar over it; this note lands
the record half, so a future variant/ADT layer (tagged records, the mixed
`list<prop>` pass) can build on records, and a shape-over-records desugaring
stays an option, without this pass merging the two.

## What this enables

Records are the foundation of the data-model spine (backlog.md), and landing
them unblocks the passes stacked on top:

- **records/props/objects unification**: with records defined as the value
  pole and objects as the actor pole, the unification pass can place props and
  Go-style receiver methods against a settled data model.
- **mixed `list<prop>`**: a `prop` boxes a tagged value; a record is a natural
  payload, and tagged records are the variant machinery that pass will want.
- **record introspection**: reflection over a record's fields (for serialize
  and pattern-match macros) needs the fields to be a first-class, named,
  ordered set, which this note establishes.
- **verbs.md UFCS (decision 4)**: activated here, since it was explicitly
  deferred "until records land".

## Survey

- **Compact Pascal / classic Pascal**: value records, nominal, no methods, the
  direct ancestor. Excelsior keeps the value semantics and nominal typing.
- **Go**: value structs, nominal, behavior by receiver funcs (not methods on
  the struct), composition over inheritance. The receiver model is exactly the
  UFCS decision here.
- **C# struct vs class**: the explicit value-type-versus-reference-type split,
  which is the record-versus-object teaching line adopted here.
- **Zig struct**: value, nominal, a struct literal names its fields (`.x = 3`)
  and function calls are positional-only; Excelsior takes the value-struct and
  positional base but **diverges** on named function arguments (nearly free once
  record literals exist, and readable with a `:` and the required-must-be-named
  safety rule).
- **Icon record**: `record point(x, y)` with positional construction
  `point(3, 4)` and `p.x` access, no keyword arguments, the positional-form and
  record-as-applied-name precedent.
- **Smalltalk keyword messages / Lisp `&key`**: named arguments as first-class
  (`x: 3 y: 4` selectors; `:x 3` pairs); the `name: value` named-argument form
  here is that lineage, in a parenthesized argument list.
- **Rust struct**: value, nominal, `Point { x, y }` construction, behavior in
  separate `impl` blocks (UFCS-adjacent). No inheritance, like here.
- **C struct**: value, no methods, inline layout. The memory model.

Excelsior's stance: Pascal/CP value records, Go/CP receiver funcs by UFCS (no
methods on the record), nominal value typing, a type-name constructor with
named fields (Swift), and the C# value-versus-reference distinction as the
record-versus-object teaching split.

## Decisions (confirmed)

The eight decisions are confirmed; the implementation (record construction,
field access, value-copy semantics, lowering, and the UFCS dot-resolution
order) is the follow-up. The named-field separator (`:` versus `=`) is settled
during implementation.


**D1. A record is plain data: named, typed fields with value semantics.** No
identity, no verbs, no dispatch, no inheritance; copied on assignment and
by-value pass. Fields are all public (a record has nothing to encapsulate) and
may carry defaults.

**D2. Record versus object is the data-versus-actor split.** A record is a
value you hold and copy; an object is an actor you send messages to. This is
the teaching rule and the guidance for which to reach for.

**D3. Construction is the type name applied, `Point(...)`, parallel to
`spawn(Class)`.** Named fields `Point(x: 3, y: 4)` are the default,
order-independent and default-friendly; a positional form is allowed for short
records. A word, not a delimiter. A record literal names its fields with
`name: value` (`Point(x: 3, y: 4)`), and the **same `name: value` form is a named
function or verb argument**, because a record literal and a call parse
identically (an applied name with an argument list, disambiguated by resolution),
so supporting named arguments is nearly free once record literals exist. The
separator is **`:`** (not `=`): a named argument must never read as an assignment
nor sit one keystroke from `==`, which is where the Swift/Python colon earns its
place, on readability, not mimicry. Positional `Point(3, 4)` and positional calls
remain (the Icon/Delphi/Zig form). A **safety rule** guards the named form: in a
call or literal that uses named arguments, every parameter or field **without a
default must be named**, so a named call cannot silently skip a required one; a
builtin, `spawn`, or a send takes positional arguments only.

**D4. Field access is `p.field` (read/write); behavior is UFCS.** Dot
resolution order is field, then verb (classes only), then UFCS func, so
`p.norm()` is `norm(p)`. Records carry no funcs of their own. This activates
verbs.md decision 4.

**D5. Records are nominal, value-equal, and cross a send by copy.** A named
nominal type usable as field, local, parameter, return, and list-element type;
two records are equal when all fields are; a record marshals as data across
the actor boundary, which is what makes it the multiple-return and
cross-actor-data vehicle. Structural/interface typing stays the deferred
interface pass.

**D6. Records are inline, fixed-size, and need no GC of their own.** They live
in their container and are copied shallowly; the immutable-string /
copy-on-write-list / shared-obj-handle model makes the shallow copy safe. A
self-referential record needs an `obj` or list indirection.

**D7. Records stay distinct from shapes; a `shared` parameter is the by-ref
opt-in.** A shape validates story-writer `[...]` data; a record is a mechanics
struct. Records are the machinery a future variant/ADT and a shape-desugaring
could build on, not merged here. To mutate a caller's record in place, pass it
`shared` (shared-params.md), the single explicit by-reference path.

**D8. Records are designed to be compile-time introspectable for macros.** The
minimal primitives are a field-presence boolean, a field-type check, and field
iteration over (name, type) pairs, all compile-time with zero runtime cost
(the Zig / D / Nim model, not Go's runtime reflection), and Meta-tier. The
exact spelling is the deferred record-introspection pass; this note unblocks it
by fixing that a record's fields are a first-class, named, ordered set the
compiler can be asked about.
