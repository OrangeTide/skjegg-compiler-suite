# Typed macros: compile-time type dispatch and the `_Generic` tier

Status: decided (2026-07); implemented (D1-D6). **D3** (`typeof(x) = T`,
`typeof(a) = typeof(b)`, and `match typeof(x)` with type labels, all nominal
and all at expansion time), **D4** (`${T}` type holes, in the type positions a
quoted expression has), and **D6's `error(...)` arm** are in, and D1 and D5
follow from them: a generic operation is a macro monomorphized per call site,
and its caller writes an ordinary call. **D2** is the phase all of it runs in:
expansion happens inside the checker, with the arguments checked first and the
expansion checked in place. The equality spelling in this note was `==`,
retired by equality.md, and is corrected to `=` throughout. See the
implementation notes at the end.
The typed-macro pass from the backlog (backlog.md), the tier
meta.md, record-introspection.md (`typeof` "underpins the `_Generic` tier"),
typed-data.md (stage 4, `${T}` holes), and inline-for.md all defer to. Tier:
**Meta** (tier 3, library authors); a generic operation is *invoked* like an
ordinary call, so its use is invisible, only its definition is Meta. Builds on the
untyped macro model (meta.md: `macro ... endmacro`, expand-then-typecheck), the
meta primitives (record-introspection.md: `typeof`/`fieldtype`/`fieldsof`), the
compile-time `match` (match-when.md), and monomorphization (inline-for.md).

The base macro model (meta.md) is **untyped**: a macro expands before
typechecking and only rearranges forms, so it cannot ask "is this argument an int
or a str?". The base layer also has no parametric polymorphism (type-parameters.md
retired `<T>`; functions are monomorphic, function-values.md). So there is no way,
yet, to write a generic `max`, `abs`, `serialize`, or container helper. This pass
lands the tier that supplies it: a **typed macro** that inspects its arguments'
static types and emits the matching monomorphic code. Genericity is a Meta-tier
facility, not a base-layer feature, which keeps the base type system small and
pushes generics to library authors (tiers.md).

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

## The ordering problem, and the phase that solves it

A typed macro needs a contradiction resolved: **macros expand before
typechecking, but a typed macro's decision depends on a type, which typechecking
produces.** The base model runs macros first (structural), then checks the
result. Type information does not exist yet when a structural macro runs.

The resolution is a **second phase**: a typed macro **resolves during
typechecking, not during the initial structural expansion.** When the checker
reaches a typed-macro invocation it has already checked the argument
*expressions*, so their types are known; it then runs the macro's type-directed
logic with those types in hand and checks the monomorphic body the macro emits.
This is C's `_Generic` (resolved at type-analysis time, not by the untyped
preprocessor) and a template's second-phase instantiation.

The discipline that keeps this sound and terminating: **type flows only inward,
from already-checked arguments into the macro; the macro's output is then
checked; a macro cannot influence the types of its own inputs.** That acyclicity
means typing never chases its own tail, and the record-introspection.md
recursion-depth cap bounds a macro that calls macros. So there are two macro
phases, the untyped structural expansion of meta.md and this typed,
typecheck-time resolution on top of it.

## `typeof` and compile-time type dispatch (the `_Generic`)

The bridge is **`typeof(expr)`** (record-introspection.md), a value taken to its
static type as a compile-time value. Over type values the macro dispatches, and
it reuses surface the language already has:

- **`typeof(x) = T`** is the compile-time single-type test (type-value
  equality against a type literal), a compile-time bool.
- **`match typeof(x)`** is compile-time multi-way dispatch, reusing match-when.md
  with a type value as the subject and type literals as labels:

      macro max(a, b)
          match typeof(a)
              when int, decimal then quote( ${a} > ${b} then ${a} else ${b} )
              when str           then quote( longer(${a}, ${b}) )
              otherwise              error("max needs a number or a str")
          endmatch
      endmacro

`match typeof(a)` chooses **one** branch at compile time and emits only its body,
so the dispatch has **zero runtime cost**, this is the C `_Generic`. It is the
compile-time twin of the *runtime* `match x` over an `any of (...)` union
(union-types.md, which compares box tags at run time): compile-time type dispatch
where the type is statically known, runtime type dispatch where it is not.

## `${T}` typed holes: type-parametric templates

A template macro's quasiquote gains a **type hole `${T}`**, splicing a type value
into the emitted forms so the template is parametric over a type
(completing typed-data.md stage 4):

    macro pair(T)                       // T is a type value
        quote(
            record Pair
                first  is ${T}
                second is ${T}
            endrecord
        )
    endmacro

Here `${T}` places the type into a `record` the macro emits. A type hole is the
same "a type in a hole" idea as typed-data.md's typed shape slot; both let a
template be filled with a type rather than a value.

## Monomorphization, and the reflection it completes

A generic operation is a **macro monomorphized per call site**: each `max(a, b)`
expands against *its* arguments' types, so different sites get different, fully
typed code, exactly template instantiation. The invocation reads like an ordinary
call, so a content author calling `max(hp, cap)` never knows a macro is behind it;
only the library author who wrote it works at Meta tier.

Typed macros **compose with the reflection primitives** into full compile-time,
zero-runtime generic code. A serializer walks a record's fields with inline-`for`
over `fieldsof(T)` (record-introspection.md, inline-for.md), and each field's body
dispatches on `f.type` with `match typeof`, emitting the per-type encoder:

    macro serialize(value)
        for f in fieldsof(typeof(value))        // unrolled, one copy per field
            match f.type
                when int then ...
                when str then ...
                ...
            endmatch
        endfor
    endmacro

`fieldsof`/`fieldtype`/`typeof` supply the structure, `match typeof` the
per-type code, inline-`for` the unrolling, all the **same body-synthesis
machinery**, and all resolved at compile time with no runtime type metadata (the
Zig/D/Nim stance, not Go `reflect`, per record-introspection.md and memory.md).

## No bounds system: requirements are checked on expansion

Excelsior does **not** add a formal constraint or bounds system (no `where T is
Comparable`, no typeclasses). A typed macro's requirements are enforced the way
meta.md already checks everything, **on the expansion**: if `max`'s emitted `>`
does not type for the argument, the base checker rejects the expansion
(expand-then-check). The quality lever is that a good macro checks explicitly and
errors clearly, an `otherwise error("...")` arm in its `match typeof`, or the
fallible `fieldtype` (record-introspection.md) failing with a message, rather than
letting a raw post-expansion type error surface. This is the C++/Zig model
(structural, an error on instantiation) rather than Rust's (declared, checked
bounds), chosen because it needs no new type-system machinery and reuses the
meta layer's existing expand-then-check and clear-error patterns.

## What is deferred

- **A formal constraint / bounds system** (declared, checked `where` clauses and
  typeclass-style interfaces), a larger type-system pass if the structural model
  proves too loose.
- **Richer type reflection** (the type of a type, constructing new generic types
  beyond a `${T}` splice, variance), additive as needs appear.
- **Caching / deduplicating** identical monomorphic instantiations across call
  sites, a compile-time-cost optimization, not a surface concern.

## Survey

- **C11 `_Generic`**: type-directed selection resolved during type analysis, not
  by the untyped preprocessor; the exact phase split and the `match typeof` model.
- **Zig `comptime`**: types as first-class compile-time values a function
  inspects and dispatches on; the `typeof`/type-value model and the
  compile-time-zero-runtime stance.
- **C++ templates**: monomorphization per instantiation with duck-typed,
  error-on-instantiation requirements; the structural (no-bounds) choice.
- **Rust generics / traits**: declared, checked bounds; the road not taken, its
  machinery being more than the base type system should carry for a Meta-tier
  facility.
- **Template Haskell / Terra / MetaOCaml**: staged, typed metaprogramming where a
  later phase has type information the earlier lacked, the two-phase model.

Excelsior's stance: no base-layer generics; a Meta-tier typed macro resolved at
typecheck time inspects argument types via `typeof`, dispatches with `match
typeof` (the `_Generic`) and splices types with `${T}` holes, monomorphizes per
call site, composes with `fieldsof`/inline-`for` into compile-time reflection, and
enforces requirements by checking its expansion, all with zero runtime type
metadata and no new base type-system machinery.

## Decisions (confirmed)

The seven decisions are confirmed. The implementation (the typecheck-time
resolution phase for typed macros, `typeof` and `match typeof` compile-time
dispatch, `${T}` type holes in quasiquotes, monomorphization per call site
composing with `fieldsof`/inline-`for`, and expansion-checked requirements)
follows the untyped macro tier (meta.md), which lands first; a formal bounds
system and richer type reflection are deferred follow-ons.

**D1. The base layer has no parametric polymorphism; genericity is a Meta-tier
facility built from typed macros.** A typed macro inspects its arguments' static
types and emits monomorphic per-type base forms the checker validates, keeping the
base type system small and pushing generics to library authors (tiers.md). A
generic operation is invoked like an ordinary call; only its definition is Meta.

**D2. A typed macro resolves during typechecking, not during the initial
structural expansion.** The checker checks the argument expressions first (their
types are then known), runs the macro's type-directed logic, and checks the
emitted monomorphic body. Type flows only inward from checked arguments, and a
macro cannot influence its own inputs' types, keeping typing acyclic and
terminating (with the record-introspection.md recursion cap). This is the second
macro phase above meta.md's untyped structural expansion.

**D3. `typeof(expr)` is the value-to-type bridge; `typeof(x) = T` is the
compile-time single-type test and `match typeof(x)` is compile-time multi-way
dispatch** (reusing match-when.md, one branch chosen, zero runtime cost). This is
the `_Generic`, the compile-time twin of the runtime `match x` over an `any of
(...)` union (union-types.md).

**D4. A `${T}` typed hole splices a type into a quasiquote template**, making a
template macro type-parametric (`record Pair ... first is ${T} ...`). This
completes typed-data.md stage 4, whose typed shape slot is the same "a type in a
hole" idea.

**D5. A generic operation is a macro monomorphized per call site, composing with
`fieldsof`/`fieldtype` and inline-`for` into compile-time, zero-runtime
reflection** (serialize/clone/compare any record). It is the same body-synthesis
machinery as inline-for.md, with no runtime type metadata (the Zig/D/Nim stance).

**D6. There is no formal constraint/bounds system; a typed macro's requirements
are enforced on its expansion (expand-then-check).** Clear errors are written
explicitly (an `otherwise error(...)` arm, the fallible `fieldtype`) rather than
declared as bounds. This is the C++/Zig structural model, not Rust's bounded one,
chosen to add no new type-system machinery.

**D7. The meta.md guardrails hold.** Meta-tier library-authors-only; **no new
power** (a typed macro emits base forms the base checker validates, so it computes
over types to generate well-typed code and cannot bypass the type system);
compile-time only with zero runtime type metadata; terminating by acyclic
type-inflow and the recursion-depth cap.

## Implementation notes

**Type comparison landed first, ahead of the rest of the tier.** It is the
smallest piece of D3 and the one meta-values.md left waiting: that pass widened
`typeof` to accept an atom, but nothing could consume a type, because `=` was
scoped to atoms. `typeof(x) = T` and `typeof(a) = typeof(b)` close that, so a
macro can dispatch on a type today with a meta `if`, before the full
typecheck-time resolution phase (D2) and `match typeof` exist.

**Comparison is nominal**, the question this note had left open. Two types are
the same when they are the same declaration, so two records with identical
fields are different types, matching records.md's nominal rule rather than
introducing a structural notion at the Meta tier alone. A builtin compares by
its kind, a `list of T` and a `set of E` compare through to the element type,
and a `maybe T` is compared through, since a fallible value is still a value of
T for the purpose of choosing code.

**Naming a type in a macro body** needed two small pieces:

- A **builtin type keyword is an expression inside a macro body only**
  (`in_macro` in the parser). Making it one everywhere would change what a bare
  `int` means in ordinary code, which is not worth a Meta-tier convenience.
- A **declared record, class, or enum name is annotated, not resolved**. A
  macro body is deliberately unresolved, since its names are meta names, so the
  resolver only stamps the symbol on a name that matches a declared type and
  errors on nothing. A meta binding of the same name still wins, because the
  expander looks in its own environment first, which `exs_meta_typecmp` pins
  with a macro whose parameter is named after a record.

**Still open for the `match typeof` half**: match labels are constants today,
so type labels need the label checker to accept a type value, and the
exhaustiveness question (is a type match ever exhaustive?) has no answer yet.
That work belongs with D2's resolution phase, not with this increment.

**`match typeof` landed next, and needed no new dispatch machinery.** A meta
`match` evaluates its subject and its labels as meta values and compares them
with the same rules, so a type subject with type labels is one case of a
construct that also takes number, text, and bool subjects. Both forms work:
the statement (arms are statement lists) and the expression (an arm body is one
expression). A value list is unchanged, a `lo to hi` range is numbers only
since a range wants ordering, and a named type reaches a label as an ordinary
identifier, resolved by the same stamped symbol type comparison uses.

The exhaustiveness question this note left open is answered by **not** asking
it. A type match is not exhaustive over any closed set, so instead **a meta
`match` that matches no arm and has no `otherwise` is an error at the match**.
The base `match` statement is a no-op in that case, which is right for a
runtime statement and wrong here: a macro that quietly emits nothing surfaces
later as "produced no form", naming the wrong thing entirely.

**`error(...)` reports at the call site.** D6 rests on a macro rejecting an
argument in its own words, so the message has to reach the author who wrote the
argument, not point into a library macro's body. The expander tracks the call
being expanded and reports there.

**What D2 would still buy.** Everything above runs during the untyped
structural expansion, reading types off arguments the checker has already
checked. That is enough for dispatch on an argument's type, which is the
`_Generic` case. The resolution phase matters for the cases where the macro's
own output must be checked before a later expansion decision depends on it,
and for `${T}` holes emitting a type into a declaration. Neither is needed by
the dispatch that works today.

**`${T}` landed in the type positions a quoted expression has.** `quote` takes
an expression, so the type positions available today are the cast (`${v} as
${T}`) and, through it, any conversion a macro emits. D4's own example emits a
`record` declaration, which needs two things this increment does not have: a
`quote` over a declaration, and a macro call at top level for the declaration
to land in. Those belong with the declaration-macro work meta.md defers
(mixins), not with the hole itself. The hole mechanism is general: it
substitutes into any type the template writes, including through the element of
a `list of ${T}`.

**A type reaches a macro three ways**: as a builtin named at the call site
(`convert(3, float)`, accepted in an argument position where a type completes
the argument, so nothing else changes shape), as a record, class, or enum name
(an ordinary identifier the resolver already stamps), or from `typeof` inside
the body. All three bind the parameter to a type value rather than a form, so
the macro sees the same thing however the type arrived. A type argument that
reaches an ordinary call is a teaching error, since the spelling now parses
where it did not before.

**This uncovered a soundness hole in the meta layer.** `form_build` clears a
copied node's type so the spliced form re-checks at the call site, which is
right for an *inferred* type and wrong for a *written* one. A quoted `${v} as
float` had its cast type cleared, so it became a cast to `any`, and `var d is
str = tofloat(3)` typechecked. Written types are now carried through
expansion; `exs_err_quoted_cast` pins it.

**A macro argument is a decimal boundary.** An untyped decimal constant
(numbers.md's ET_DEC) takes its default when it binds to a macro parameter,
so a macro asking whether `9.5` is a decimal is told yes. Without that,
`match typeof(v)` over a literal argument silently missed every arm.

**D2 was already the structure, and completing it was two fixes.** Expansion
never ran as a separate structural pass: `expand_macro` is called from the
checker's call case, which checks the arguments first (so their types are known
to `typeof`), runs the macro, and checks the emitted form in place. That is the
second phase this note describes, so D2 needed no phase to be built. What it
did need was for the phase to be complete in two ways the dispatch cases had
not exercised.

**An emitted form is now resolved at the call site.** A form a macro built was
never parsed in a scope, so a name it writes carries no symbol, the resolver
having run before the form existed. Splices were fine, since a spliced argument
arrives already resolved, and so were builtins and sends, which need no symbol.
A name the macro body *writes* was not: a macro emitting a call to another
macro, or to a sibling func of the class it expands into, produced a call that
reached lowering unresolved. The checker now looks such a callee up where the
expansion landed, a sibling member first and then module scope, which is
ordinary lookup order. This is what makes a macro able to build on a macro.

**The depth cap had to move.** It counted expansion, but a macro that emits a
call to itself recurses through the *checker*, so the count was back to zero by
the time the recursion happened and a non-terminating macro ran the C stack out
instead of reporting. The cap now wraps the expansion and its re-check
together. This is the bound D2's termination argument rests on, so it is worth
the test that pins it (`exs_err_meta_deep`).
