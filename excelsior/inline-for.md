# Inline `for`: iterating a fixed sequence of mixed types

Status: decided (2026-07), not yet implemented. A compile-time-expansion pass
from the backlog (backlog.md, TODO line 26), the machinery
record-introspection.md (D4) and mixed-lists.md (D6) both defer to. Tier: Mechanics (a systems author iterates a mixed literal),
and the machinery under the Meta facilities (a macro's `fieldsof` iteration,
record-introspection.md). Relates to typed-data.md and tiers.md.

An ordinary `for` iterates a sequence whose elements share one type: `for x in
someList` binds `x` at the list's element type, once, and loops at runtime. But
a **fixed sequence of mixed types**, a heterogeneous literal `[1 "two" 3.0]` or a
record's fields `(x is decimal, y is decimal)`, has no single element type, so a
runtime loop cannot type its body. Iterating it means **expanding the body once
per element** with the loop variable bound to that element's concrete type. This
is Zig's `inline for` and D's `static foreach`, and it is the shared engine
under `fieldsof` iteration and mixed-data processing.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

## The problem: a fixed sequence with no single type

Given `[1 "two" 3.0]`, what is the type of `x` in `for x in ...`? There is none:
element 1 is int, 2 is str, 3 is decimal. Two ways out:

- **Box every element into `any` and loop at runtime**, narrowing each with
  `match` (mixed-lists.md). This works, but it boxes (a runtime cost) and forces
  a `match` even when the sequence is a fixed, compile-time-known literal.
- **Unroll the loop**, emitting the body once per element with `x` at that
  element's concrete type. No boxing, no `match`, and each copy is checked
  against a real type. This is the right tool when the sequence is
  compile-time-known, and it is the only way to iterate a record's fields, whose
  types are genuinely different.

Inline `for` is the second. It is not a separate keyword; it is what `for` does
when its head is a fixed, compile-time sequence of mixed types.

## When `for` unrolls

A `for` **unrolls** (expands per element, static) exactly when its head is a
**compile-time-known sequence of statically-typed elements**:

- a **heterogeneous data literal used directly as the head**, `for x in [1
  "two" 3.0]` (never materialized into a runtime list; see the boxing
  alternative below);
- a **const-bound** such literal;
- a **compile-time meta-sequence**, `for f in fieldsof(T)` (and later
  `membersof`, `verbsof`), whose elements are a record's fields
  (record-introspection.md).

A `for` **loops** (ordinary runtime) for everything else: a homogeneous literal
`[1 2 3]` (one element type), a runtime `list`/`buffer`, or a range `1 to 10`.
The loop variable there has one static type, so no unroll is needed or observed.

The unroll is **not opted into with an `inline`/`static` keyword**, unlike Zig
and D. In those languages a homogeneous array can be looped *or* unrolled, so
unrolling is a comptime choice that needs marking. In Excelsior it is not a
choice: a fixed sequence of mixed types has no uniform element type, so a runtime
loop over it is impossible, and unrolling is the only lowering. The author writes
`for`, and it expands exactly when it must. This keeps the surface small and
matches "observable only for mixed data" (backlog): for homogeneous data,
unroll-versus-loop is indistinguishable, so the compiler loops.

**The boxing alternative is opt-in.** A heterogeneous literal that flows into a
`list of any` (stored in a field, passed, or ascribed `list of any`) becomes a
runtime boxed mixed list, iterated with `match` (mixed-lists.md). So iterating a
mixed literal *directly* unrolls (efficient, concrete types); *materializing* it
first boxes. The two are the fixed-and-known versus stored-and-runtime cases.

## Per-element static typing

The core is that **each unrolled copy is monomorphic**: in the copy for element
`i`, the loop variable has element `i`'s concrete type, and the body is
typechecked against that type. So `for x in [1 "two"]` emits the body once with
`x` an int and once with `x` a str, and each copy must be valid for its type.
This is what makes iterating a record's fields work, `f.type` is a real,
different type in each copy (record-introspection.md's monomorphic-per-field).

The realistic body is one valid for each element type: an interpolated `${x}`
(the type router handles any type), a `match x` / `x is T` narrowing inside, or
a call to something accepting each type. When a copy is **not** valid for its
element's type, the diagnostic **names the element and its type** (backlog):
`in the iteration for element 2 (a str), x + 1 has no int`. That per-element
error, impossible for a runtime loop, is the point of typing each copy.

## Bounded, static, and jump-aware

The unroll is **fully static**: no loop counter and no runtime iteration, just N
copies of the body inlined, so there is zero loop overhead. Heterogeneous
literals and field sets are small by nature, so N is small; a **size cap** (an
extreme maximum, the twin of record-introspection.md's recursion-depth cap)
backstops a pathological unroll with a clear error rather than code-size blowup.
Nesting is limited to one level of unroll for the same reason (mixed-lists.md
D6). `break` and `continue` work as compile-time-resolved jumps between copies
(`continue` to the next copy, `break` past all remaining), so an unrolled loop
still reads like a loop.

## The runtime sibling: type-dispatch over `list of any`

The same "synthesize a body per static type" machinery has a **runtime** face,
which lands mixed-lists.md's deferred D6. A `for x in xs` over a **runtime `list
of any`** cannot unroll (the length and element types are unknown), but the tag
universe is closed (int, bool, decimal, float, str, list, a record, an obj,
nothing/nil). So the body is **synthesized once per reachable tag**, and the
runtime loop dispatches each element to its tag's copy, a type-case with no
`match` written by hand:

    for x in mixed              // mixed is list of any
        tell(player, "${x}")    // one body, synthesized per tag, dispatched at runtime
    endfor

This is a runtime loop whose body is a compile-time-synthesized tag switch,
bounded by the ~8-tag universe, one nesting level, with failing-tag diagnostics
(mixed-lists.md D6). It is the convenience over the explicit `match x` inside a
`for` that mixed-lists.md D5 keeps as the core; both narrow, one by hand and one
synthesized. The static unroll and this runtime dispatch are the two faces of one
mechanism: unroll when the head is compile-time-known, synthesize-and-dispatch
when it is a runtime `list of any`.

## What is deferred

- **Multi-level unroll nesting** beyond one level; bounded now, additive later.
- **Unrolling a runtime sequence** is not a thing (a runtime head has unknown
  length and types); only compile-time heads unroll, runtime `list of any` heads
  get the tag dispatch.
- **A user-visible `inline`/`static` marker**; excluded by design, since the
  unroll is forced by the data, not chosen.

## Survey

- **Zig `inline for`, D `static foreach`**: the direct model, a loop unrolled at
  compile time with each element statically typed; Excelsior drops the explicit
  keyword because the unroll is forced (mixed fixed sequences have no uniform
  type), not an optimization choice.
- **Nim `for` over tuples, `fieldPairs`**: iterating a tuple or a record's fields
  unrolls because a tuple is heterogeneous, the same fundamental reason; the
  `fieldPairs` field walk is record-introspection.md's `fieldsof`.
- **C++ fold expressions and (C++26) expansion statements**: `template for` over
  a pack unrolls per element with per-element types, the language finally adding
  what Zig/D had.
- **Rust**: no direct heterogeneous-`for`; tuple iteration goes through macros or
  const generics, the gap this feature fills directly.
- **Julia `ntuple` / generated functions**: compile-time expansion producing
  per-index typed code, the metaprogramming route to the same effect.

Excelsior's stance: Zig/D compile-time loop unrolling for a fixed sequence of
mixed types, made implicit (forced by the data, no keyword), monomorphic per
element with element-naming diagnostics, plus a runtime tag-dispatch sibling for
`list of any`, all one body-synthesis engine.

## Decisions (confirmed)

The six decisions are confirmed. The implementation (detecting a compile-time
sequence head, the per-element monomorphic body expansion with element-naming
diagnostics, the size and nesting caps, the compile-time `break`/`continue`
jumps, and the runtime tag-dispatch lowering over `list of any`) follows; it
shares machinery with record-introspection.md's `fieldsof` iteration and lands
mixed-lists.md D6.

**D1. A `for` unrolls when its head is a compile-time-known sequence of
statically-typed elements** (a heterogeneous literal used directly as the head, a
const-bound one, or a meta-sequence like `fieldsof(T)`), expanding the body once
per element. Everything else (homogeneous literal, runtime list/buffer, range)
is an ordinary runtime loop.

**D2. There is no `inline`/`static` keyword; the unroll is implicit.** Unlike
Zig/D, it is forced by the data (a fixed sequence of mixed types has no uniform
element type, so a runtime loop is impossible), not an opt-in optimization, so
`for` expands exactly when it must and is otherwise a loop.

**D3. Each unrolled copy is monomorphic:** the loop variable has that element's
concrete type and the body is typechecked against it. A copy invalid for its
element's type is a compile error **naming the element and its type**, the
per-element diagnostic a runtime loop cannot give.

**D4. The unroll is fully static and bounded.** N copies inlined, no runtime
counter; a size cap (the twin of record-introspection.md's recursion cap) and a
one-level nesting limit backstop pathological expansion; `break`/`continue` are
compile-time-resolved jumps between copies.

**D5. Iterating a mixed literal directly unrolls; materializing it into `list of
any` boxes.** Direct iteration is the efficient concrete-typed path; storing or
passing the literal as `list of any` is the opt-in runtime-boxed path
(mixed-lists.md).

**D6. The runtime sibling lands mixed-lists.md D6:** a `for` over a runtime `list
of any` is a runtime loop whose body is synthesized once per reachable tag (the
closed ~8-tag universe, one nesting level, failing-tag diagnostics), the
auto-`match` convenience over the explicit `match x` mixed-lists.md keeps as the
core. Static unroll and runtime tag-dispatch are the two faces of one
body-synthesis machinery.
