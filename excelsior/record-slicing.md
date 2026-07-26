# Record slicing: `p with (x, y)`, a field subset

Status: decided (2026-07); implemented (D1-D6). A records follow-on (records.md),
the field-subset counterpart to array slicing. Tier: Mechanics; slicing a record is a systems-author tool for
partial comparison and partial update. Builds on records (records.md: value
semantics, nominal typing, value equality), the by-reference `shared` parameter
(shared-params.md), and Pascal's record `with`.

An array can be sliced to a range of its elements; a record should be sliceable
to a subset of its fields. **`p with (x, y)`** names record `p` restricted to the
fields `x` and `y`, for three things a whole record makes clumsy: comparing two
records on only some fields, updating only some fields, and handing a function
access to only some fields. The design keeps it cheap by making the field subset
**a compile-time access rule, not a runtime value**: the field names are literal,
so the compiler emits exactly the operations the subset allows, with no runtime
mask and no copy.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

## The word is `with`

A record slice is spelled **`p with (x, y)`**. `with` is the word Pascal already
uses to scope a record's fields (`with rec do ...`), it reads as plain English
("`p`, with `x` and `y`"), and it takes no new symbol. The `[...]` of array and
string slicing is deliberately *not* reused: that slicing is by **position**
(`s[2 to 4]`), while a record slice is by **field name**, a different thing that
deserves a different spelling.

## Same type only

A slice is of a specific record type, and a comparison or assignment is between
**the same record type**. `a with (x, y)` and `b with (x, y)` slice the same
record type; two *different* record types that happen to share `x` and `y` do not
compare, because records are **nominal** (records.md) and slicing does not open a
structural back door. This keeps the type story the one records already tell and
the implementation simple; a structural, cross-type field view is the deferred
interface/row-polymorphism work, not this.

## In-place: the compiler emits the field operations

The primary use is in place, and it is resolved entirely at compile time because
the field names are literal. **Assignment through a slice is a subset
modification**, touching only the named fields:

    p with (x, y) = source        // p.x = source.x; p.y = source.y  (p.z untouched)

The compiler emits exactly the per-field copies for `x` and `y` and no others, so
the unnamed fields are protected from the write by simply not being written, the
"mask off the rest" the slice promises. **Comparison through a slice compares only
the named fields**:

    if a with (x, y) = b with (x, y) then ...   // a.x = b.x and a.y = b.y

which the compiler emits as the conjunction of the named-field comparisons,
ignoring the rest, a natural extension of the whole-record value equality
(records.md D5). Neither form needs a runtime mask: the subset is unrolled into
concrete field loads and stores at compile time.

## First-class through a field-restricted parameter

A slice is otherwise **in-place**, not a stored value, but it becomes usefully
first-class in one place, **a function parameter**, without any copy or runtime
mask. A parameter typed `p is Point with (x, y)` is a **by-reference view**
(shared-params.md, so the whole record is passed by reference, not copied) whose
body **may access only `x` and `y`**, enforced by the type checker:

    func recenter(p is Point with (x, y))
        p.x = 0                    // ok
        p.y = 0                    // ok
        // p.z here is a compile error: outside the slice
    endfunc

    recenter(somePoint)            // a whole record narrows to the slice
    recenter(other with (x, y))    // or a slice passes through

You may pass a whole record or a slice; either way the callee is confined to the
masked fields, checked at compile time. This is the sparse intent met cheaply: the
runtime passes the record as-is (a reference), the field restriction costs nothing
at run time, and no copy is made. It is a `shared` parameter (shared-params.md)
with a field-access mask on top.

## Lifetime: call-scoped, like `shared`

A slice binding (the restricted parameter, or a local naming one) is a
**call-scoped, non-escaping reference** to its record, exactly the shared-params.md
rule: it does not outlive the call, is not stored in a field or returned, and
never crosses the actor boundary. That is what keeps it safe with no runtime
representation, the record it views owns the data and outlives the slice, so the
slice is only ever a compile-time restriction over a live record.

## Everything is compile-time

The load-bearing property is that a `with (...)` names **literal fields**, so a
slice has **no runtime form**. In-place, the compiler unrolls it into concrete
field operations; as a parameter, it is a compile-time access restriction over a
by-reference record. There is no mask word to carry, no sparse copy to perform,
and no new value kind in the runtime; the feature adds a checker rule and a
compile-time unroll, and nothing to the running program.

## What is deferred

- **Cross-type structural slices** (a `with (x, y)` view over any record that has
  `x` and `y`), which is the deferred interface / row-polymorphism axis, not this
  nominal feature.
- **A stored or returned first-class slice** (escaping its call), which would need
  the memory-management lifetime model (memory.md); the call-scoped form needs
  nothing.
- **Slicing a nested record's fields** (`p with (pos.x)`) and read-only versus
  read-write slice parameters, additive refinements.

## Survey

- **Pascal `with`**: scopes a record's fields in a block; the word and the
  field-scoping idea reused here as a first-class subset.
- **SQL projection** (`SELECT x, y FROM ...`): choosing a column subset of a row,
  the relational analogue of a record slice; here it is compile-time and
  same-type.
- **Rust struct update `..`**: `Point { x: 1, ..p }` builds from a subset; Rust
  updates at *construction*, this masks at *assignment* (a subset write) and adds
  a restricted view, which Rust does not have.
- **Haskell lenses / functional optics**: a composable focus on part of a
  structure for get and set; the get/masked-set duality here, without the
  first-class optic (the slice is compile-time, not a value).
- **ML row polymorphism / TypeScript `Pick<T, K>`**: a structural field subset as
  a type; deferred here in favor of the nominal, same-type form.

Excelsior's stance: a record slice `p with (x, y)` is a compile-time,
same-type, field-subset access rule, used in place for subset comparison and
subset assignment (the compiler emitting the concrete field operations) and as a
field-restricted by-reference parameter (a `shared` view with a field mask), with
no runtime representation, no copy, and the call-scoped lifetime of `shared`.

## Decisions (confirmed)

The six decisions are confirmed. The implementation (the `p with (x, y)` reader,
the same-type check, the compile-time unroll of an in-place subset compare or
assign into concrete field loads and stores, and the field-restricted
by-reference parameter over shared-params.md) follows; cross-type structural
slices, a stored or returned slice, and nested-field slices are deferred.

**D1. A record slice is `p with (x, y)`**, naming a record restricted to a subset
of its fields. `with` is Pascal's record-field word and adds no new symbol;
`[...]` is not reused, as that slicing is by position and a record slice is by
field name.

**D2. Same type only.** A slice is of a specific record type, and comparison or
assignment is between the same record type; two different records that share
field names do not compare (records stay nominal). A structural cross-type field
view is the deferred interface/row-polymorphism work.

**D3. In-place is resolved at compile time.** `p with (x, y) = source` is a subset
modification the compiler emits as per-field copies of the named fields, leaving
the rest untouched; `a with (x, y) = b with (x, y)` compares only the named
fields. No runtime mask.

**D4. A slice is first-class only as a field-restricted parameter.** A parameter
`p is Point with (x, y)` is a by-reference view (shared-params.md, no copy) whose
body may access only the named fields, enforced by the type checker; a whole
record or a slice may be passed. Sparse access with no copy and no runtime mask.

**D5. A slice binding is call-scoped and non-escaping** (the shared-params.md
rule): it does not outlive the call, is not stored or returned, and crosses no
send, so it needs no runtime representation and its lifetime is safe by
construction.

**D6. A slice has no runtime form.** Because `with (...)` names literal fields,
the compiler unrolls the in-place operations into concrete field loads/stores and
enforces the parameter restriction at compile time; there is no mask value, no
sparse copy, and no new runtime value kind.
