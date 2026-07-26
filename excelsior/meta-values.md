# Meta values: what a macro holds, and what it computes

Status: decided (2026-07); implemented (D1-D5, D7; D6 is a boundary, kept by
not building what it excludes). The implementation notes at the end record one
finding: D3's bridge has no consumer yet. The pass on the backlog's "a literal
is not a meta value" item, raised 2026-07 while pinning the meta guards with
`.experror` tests.
Tier: Meta (tier 3); nothing here is visible to a story or map builder
(tiers.md). Builds on meta.md (the macro surface and its guardrails) and
record-introspection.md (the primitives a macro calls); typed-macros.md
depends on the answer, since a monomorphizing macro computes with constants.

The symptom is small. In a macro body, `typeof(1)` reports "this form is not
valid in meta-layer code". Behind it sits a question the meta layer has not
answered: what is a meta value? Today the answer is an accident of which
cases `meta_eval` happens to handle, and the accident is visible as an
asymmetry between numbers, text, and bools.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

## The gap, and the asymmetry that shows it is unintentional

A string literal is already a meta value, and a bool literal is too. Both
evaluate, bind to a meta `var`, and splice:

    var s = "abc"                 // ok: a meta text value
    var b = true                  // ok: a meta bool
    quasi ${s}                    // splices as a string literal

A number literal is not:

    var n = 1                     // error: this form is not valid in
                                  // meta-layer code

Nothing motivated that split. Text and bool became meta values because the
tag-reading increment needed them (record-annotations.md D6: compare a tag's
head symbol, test membership, branch on the result). Numbers were not needed
that day, so they were not added. The same accident shows in `typeof`, which
takes a form and rejects an atom, so `typeof("x")` fails even though `"x"` is
a perfectly good meta value:

    typeof("x")                   // error: typeof(...) needs a value

A Meta author reads those three behaviors and cannot derive a rule. That is
the actual defect. The fix is to say what the value domain is, and make the
primitives agree with it.

## The domain: atoms, forms, and the descriptions of types

A meta value is one of three families, and naming them is most of the work:

- **Atoms**: a number, a text, a bool. Self-evaluating, comparable,
  computable, and spliceable as the literal they are.
- **Forms**: a piece of program, from an auto-quoted macro argument or from
  `quote` / `quasi`. Opaque to arithmetic; spliced into output.
- **Descriptions**: a type, a sequence, a field or verb or member descriptor.
  Produced by the introspection primitives, walked by a meta `for`.

The rule a reader can hold is one sentence: **an atom is a value the macro can
compute with, a form is a value the macro can only carry and emit.** Both
splice; only atoms do arithmetic; only forms have a static type to ask about.

That rule already describes text and bool. Adding number to the atom family
completes it rather than extending it, so the change is one value kind and
one operator table, with no new concept for an author to learn.

## `typeof` accepts an atom

`typeof(v)` bridges a value to its type. Given a form it yields the form's
static type, which is the case introspection needs. Given an atom it should
yield the type that atom would have in the base layer: `typeof(1)` is `int`,
`typeof("x")` is `str`, `typeof(true)` is `bool`.

This closes the `typeof("x")` hole and makes the bridge total over the value
domain, so a macro that receives either an argument or a literal it wrote
itself can treat them alike. The error message survives for the one case that
is still wrong, a type where a value belongs (`typeof(typeof(v))`), which is a
category mistake and not a gap.

## Splicing an atom is splicing its literal

`${}` already splices a text as a string literal. A number splices as a number
literal and a bool as `true` / `false`, which is the same rule applied to the
rest of the family. This is what makes the arithmetic below worth having: a
macro that computes a constant can put it in the output.

    quasi ${count} + 1            // count is a meta number: `3 + 1`

A literal written **inside** `quote` or `quasi` is untouched, because it is
part of a form, not a meta expression. So nothing an existing macro does
changes; the new behavior is only reachable in meta-expression position.

## Arithmetic, and exactly where it stops

Once numbers are atoms, the operators follow. The set is deliberately the
small one:

- `+` `-` `*` `/` `%` on numbers, and `+` on text (concatenation, for a
  generated name).
- `<` `<=` `>` `>=` on numbers; `=` and `<>` extended from text to numbers
  and bools.

This is enough for the work that motivated the item: a repeat count, an index,
a field position, a generated name with a suffix. It is not a compile-time
language.

**Numbers are int.** A meta number is a 32-bit integer, matching the base
layer's `int`. Decimal is deferred: the untyped-decimal machinery (numbers.md)
buys nothing for counts and indices, and admitting it would drag the adapt-to-
context rule into the meta layer where there is no context to adapt to. `/`
truncates, as int division does, and there is no decimal context here for the
truncation trap to fire in.

**Overflow and division by zero are compile errors**, not faults. A macro runs
at compile time, so a runtime fault has nowhere to land; the author gets an
error at the macro that computed it.

## What stays out, and why the layer still terminates

The meta layer gets arithmetic. It does **not** get general compile-time
execution:

- No meta `while`. The only meta loop stays the `for` over a sequence, which
  is bounded by the sequence it walks.
- No recursion, and no user-defined meta functions. A macro calls the
  introspection primitives and emits; it does not define helpers or call
  itself.

That boundary is what keeps expansion structurally terminating: every meta
loop is bounded by a compile-time-known sequence, so a macro cannot diverge
except by expanding into another macro, which the existing depth limit already
catches. Adding arithmetic does not touch that argument, since arithmetic does
not introduce a loop. Adding `while` would, and it would trade a guarantee for
a convenience nobody has asked for yet. It stays out until a real case demands
it, and when one does, the pass to run is a termination-bound design, not a
one-line loop.

This also holds the meta.md D6 guardrails unchanged: compile-time only, no
runtime `eval`, no new power minted, base forms out.

## The error for what remains outside

With the domain named, the catch-all message can name it too. "This form is
not valid in meta-layer code" describes a closed door without saying what is
behind it. It should say what the meta layer holds, and point at `quote` for
the common case of an author who meant a piece of program rather than a value:

    a macro body computes with numbers, text, bools, types, and sequences;
    for a piece of program, write `quote ...`

## Survey

- **Lisp**: numbers are self-evaluating, and a macro computes with them
  freely; the atom/form split here is Lisp's, minus the unbounded execution.
- **Rust `macro_rules!`**: declarative macros cannot count, which is why the
  ecosystem grew recursive tt-munchers to do arithmetic by pattern matching.
  The instructive counterexample: withholding arithmetic does not remove the
  need for it, it just makes authors encode it badly.
- **Rust proc macros**: full Rust at compile time, the far pole. Excelsior
  does not follow, since that is a second language with a second runtime.
- **Zig `comptime`**: the whole language runs at compile time, bounded by a
  branch quota. Coherent, and far past what a bounded expander needs; the
  quota is the admission that termination had to be bought back once
  unbounded execution was allowed in.
- **D CTFE** and **Nim compile-time**: the same full-execution pole, with the
  same cost.
- **C++ templates**: arithmetic by type-level recursion, the shape that
  results when a macro system has computation but no ordinary values.

Excelsior's stance: the meta layer is a bounded expander with ordinary
arithmetic, not a compile-time language. Atoms (number, text, bool) are values
a macro computes with and splices as literals; forms are carried and emitted;
`typeof` bridges either to a type. Loops stay bounded by the sequences they
walk, so expansion terminates by construction.

## Decisions (confirmed)

The seven decisions are confirmed. The implementation is small and lands in
`meta_eval`: a number-literal case producing an atom, `typeof` widened to
accept one, `form_build` splicing an atom as its literal, the arithmetic and
comparison operators over atoms with the range checks, and the reworded
catch-all. D6 is a boundary rather than code, so it costs nothing now and
guards the layer later. The `.experror` tests for the guards this pass
touched (`typeof`, the catch-all) move with the messages.

**D1. A number literal is a meta value**, an atom of the meta layer alongside
text and bool. This completes a family that was partial by accident, not by
design: text and bool became meta values because tag-reading needed them, and
numbers were simply never added.

**D2. The meta value domain is atoms, forms, and descriptions.** Atoms
(number, text, bool) are self-evaluating, comparable, computable, and splice
as their literal; forms (an auto-quoted argument, `quote` / `quasi`) are
carried and emitted; descriptions (a type, a sequence, a descriptor) come from
the introspection primitives. One sentence states it: an atom is a value a
macro computes with, a form is one it can only carry and emit.

**D3. `typeof` accepts an atom as well as a form**, yielding the type the
literal would have (`typeof(1)` is `int`, `typeof("x")` is `str`). The bridge
becomes total over the domain; passing a type where a value belongs stays an
error, being a category mistake rather than a gap.

**D4. `${}` splices an atom as its literal**, extending the existing
text-splices-as-a-string rule to numbers and bools. A literal written inside
`quote` / `quasi` is part of a form and is untouched, so no existing macro
changes behavior.

**D5. Bounded arithmetic on atoms.** `+` `-` `*` `/` `%` and `<` `<=` `>` `>=`
on numbers, `+` on text for a generated name, and `=` / `<>` extended from
text to numbers and bools. A meta number is an int (decimal deferred, since
counts and indices do not want the adapt-to-context rule), and overflow or
division by zero is a compile error at the macro, not a runtime fault.

**D6. No general compile-time execution.** No meta `while`, no recursion, no
user-defined meta functions. The only meta loop stays the `for` over a
compile-time sequence, so every expansion is bounded by the data it walks and
termination is structural. Arithmetic does not weaken that argument; a loop
would, and it waits for a case that needs it.

**D7. The catch-all error names the domain and points at `quote`**, so an
author who reaches for something outside the meta layer is told what is inside
it and how to write a piece of program instead.

## Implementation notes

**D3 had no consumer at first, and now has one.** Widening `typeof` to accept
an atom left nothing to do with the result, because the only operations over a
type were `fieldsof`, `verbsof`, and `membersof`, and an atom's type is never a
record, class, or enum. What D3 bought on its own was the removal of the
asymmetry that started the pass: a value the meta layer holds can be asked its
type, and the answer is no longer "that is not a value".

The consumer is **type comparison**, which landed next as the first piece of
typed-macros.md D3: `typeof(x) = T` and `typeof(a) = typeof(b)`, nominal. It is
recorded there rather than here, since the question it settles is what a type
is compared *by*, not what the meta layer holds. D5's "two atoms of one kind"
rule is unchanged by it; types are a separate case in the same operator, not a
fourth atom.

**A cross-kind operator says so.** `1 + "x"` reports that a meta `+` needs two
atoms of the same kind, rather than "`+` is not a meta-layer operator", which
was the message before and is false: `+` exists, just not across kinds.

**The range checks are compile errors at the macro**, per D5, with tests in
`exs_err_meta_overflow` and `exs_err_meta_divzero`. `exs_meta_values` runs the
positive cases end to end, so the constants a macro computes are checked to
reach the program: a spliced number, a compile-time field count, a generated
name, a bool from a comparison, and a meta `if` that discards one branch
before the base checker ever sees it.
