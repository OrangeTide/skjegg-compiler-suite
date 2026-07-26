# `quote` leaves the surface: reification is a meta-layer facility

Status: decided (2026-07), not yet implemented. The pass on
approachability.md's R19 (`quote` leaks the meta layer into the surface).
Tiers: the builder's data face `[...]` is World (tier 1); general reification
is Meta (tier 3) (tiers.md). The four decisions are confirmed; the
implementation (removing the `quote` keyword and its `primary` arm, the
migration hint) is the follow-up, and the meta-layer reification spelling
waits on the meta layer's own design pass.

The meta layer is otherwise cleanly hidden: its macros are reader-registered
words with their own `end<kind>` terminators, not entries in the base grammar
or keyword list. `quote` is the exception. It sits in the `primary`
production, so every expression parse considers it, and in the keyword list,
so every builder sees it, for a facility that is "rare, mostly library-author
code" and does not even lower. This note removes `quote` from the base
surface, leaving `[...]` as the builder's only quoting face and reification as
a meta-layer facility spelled when that layer's surface is designed.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

## What exists today

- **`quote primary`** (T_QUOTE, `quote_form` in the grammar, N_QUOTE) is
  explicit reification: it turns a code fragment into a quoted form. It is a
  hardcoded keyword in `primary`, the grammar comment marks it "rare, mostly
  library-author code", it is **not lowered** (lower.c errors "data literals
  are not lowered yet"), and **no test uses it**.
- **`[...]` data literals** (N_DATALIT) are the builder's data face: a
  homogeneous literal is a `list<T>` value, and a literal with words or
  nesting is quoted symbolic data typed `any` (dialog trees, stat tables).
  `${expr}` holes splice computed values under the one quotation-boundary rule
  shared with strings.

So a builder already quotes **data** with `[...]`, completely and with type
checking. `quote` adds general reification of arbitrary code, which is a
meta-layer operation (the Io Message tree, the Scheme quote), and it is the
only piece of that layer wired directly into the base grammar every builder
reads.

## The design

**Remove `quote` from the base grammar and the builder-facing keyword list.
Reification is a meta-layer facility, spelled where the rest of the meta layer
lives.**

This is R19's "gate it the way macro definition is gated" made concrete.
Macro machinery is not in the base grammar; it is registered by the reader and
runs at compile time inside macro and IMMEDIATE-word contexts. `quote` belongs
in exactly that place, not in `primary`. Because the meta layer's own surface
is a later design (the deferred meta / mixin / macro pass that shared-params.md
also feeds), the concrete action now is to close the leak: `quote` leaves the
base surface, and reification is recorded as a meta-layer facility to be
spelled when that layer lands.

Nothing is lost by removing it now:

- **The builder loses nothing.** `[...]` covers every quoting need a builder
  has (data: lists and symbolic trees), with type checking `quote` never
  offered. General code reification is not a builder operation.
- **The meta author loses nothing yet.** `quote` does not lower, so it is a
  non-functional placeholder today. When the meta layer is designed, it
  defines reification properly, inside the compile-time context where a macro
  can actually consume a quoted form.

So the split falls cleanly along the layer boundary the language already
draws: `[...]` is the runtime layer's data face (tier 1, stays), and general
reification is the meta layer's (tier 3, leaves the base grammar). The
internal N_QUOTE node kind can stay reserved for the meta layer; only the
surface keyword and its `primary` arm retire.

### Migration

A stray `quote` in ordinary code gets a teaching hint: "`quote` is a
meta-layer facility and is not part of the builder surface; to quote data,
write it as a `[...]` literal." This both points at the right tool for the
common intent (quoting data) and names the boundary, the same courtesy the
other retired spellings get.

### The sources pump holds its line

R19 also examined the sources and continuation pump (else-operator.md) under
the "meta leak" heading and it passed: it is explicitly deferred and
second-class by design, not wired into the builder surface. This note ratifies
that. When `for in` over sources eventually lands, it holds the same line:
second-class, not a base-grammar entry a builder meets before they need it.

## Survey

Where homoiconic reification sits, and whether it faces the ordinary user:

- **Scheme / Lisp**: `quote` (`'x`) is core and faces every user, because the
  language *is* the meta layer, there is no separate builder tier. The
  opposite premise from Excelsior, which deliberately has two layers and hides
  the second.
- **Io**: every expression is already a Message tree; reification is implicit,
  not a keyword. Excelsior takes Io's uniform core (core.md) but keeps it
  behind the applicative surface rather than exposing the Message directly.
- **Forth**: `'` (tick) and `POSTPONE` are compile-time words used inside
  definitions, not part of the ordinary word surface. This is the model here:
  reification is a compile-time, meta-context facility.
- **Rust / Lisp macros with explicit quasiquote** (`quote!`, backquote): the
  quoting operator is scoped to macro-authoring contexts, not general
  expression syntax. The direction this note takes.
- **Template languages (`${}` interpolation)**: the one quoting-adjacent thing
  a non-programmer meets is a hole in a template, which Excelsior already has
  in strings and `[...]`. That is the builder's entire exposure to the
  quotation boundary, and it is enough.

Excelsior's stance: the builder's quotation surface is `[...]` data and `${}`
holes; general code reification is a meta-layer facility, kept out of the base
grammar and keyword list exactly as the macro machinery already is.

## Decisions (confirmed)

**D1. Remove `quote` from the base grammar (`primary`) and the builder-facing
keyword list.** It is the one direct wiring of the concatenative meta layer
into the surface every builder reads, it does not lower, and no test uses it.
Closing the leak costs nothing today.

**D2. The builder's quotation face is `[...]` data literals and `${}` holes**
(tier 1, World). These cover every quoting need a builder has, with type
checking, so removing `quote` takes nothing from them.

**D3. General reification is a meta-layer facility** (tier 3, Meta), spelled
inside the compile-time macro / IMMEDIATE context when the meta layer's
surface is designed (the deferred meta / mixin / macro pass). The internal
N_QUOTE node may stay reserved for it; the surface keyword retires now, with a
teaching hint pointing a stray `quote` at `[...]` for data.

**D4. The sources / continuation pump holds its deferred, second-class line**
(else-operator.md), ratifying R19's own finding; `for in` over sources, when
it lands, stays out of the base builder surface for the same reason.
