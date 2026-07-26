# The meta layer: macros over the form core

Status: decided (2026-07); implemented as a vertical slice (a top-level
`macro name(...) ... endmacro`, expanded in the checker by a small
meta-interpreter: meta `var`, a `for` over a `fieldsof` walk, meta
`if`/`elseif`/`else`, and `quote`/`quasi` with `${}` splices, then
expand-then-typecheck; see `tests/exs_macro_json.exs`). Deferred:
template-macro sugar, the type splice `${T}` (typed-macros.md), and mixins.
The pass that settles the
meta-layer surface,
which several notes defer to: shared-params.md (mixins for cross-object
reuse), quote.md (reification's meta-layer home), records.md D8 (introspection
for macros), and typed-data.md stage 4 (typed holes). Tier: Meta (tier 3),
restricted to runtime-library authors and invisible to story and map builders
(tiers.md).

The meta-layer *model* is already settled in core.md ("Two layers that meet at
expansion", "The form representation", "Definitions and the meta-stack",
"Where each construct lives"). This note consolidates that model and settles
the one thing left open, the concrete surface a library author writes a macro
in, so the deferred facilities have a place to land. core.md's sections stay
the deep reference; this note is the authoritative meta-layer surface.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

## The settled model, in brief

- **Two layers, one syntax of forms** (core.md). The runtime layer is the
  applicative Algol/Io surface builders write; the meta layer is a tiny
  concatenative Forth over compile-time builder stacks. They meet at
  expansion: a surface construct expands into core forms, which are then
  type-checked and lowered.
- **Everything is a form**: an atom (literal or symbol) or a compound
  `head(arg...)`, isomorphic to Scheme s-expressions and Io Message chains.
- **Quoting is almost always implicit**: a definition body between a keyword
  and its `end<kind>` is auto-quoted (the Forth colon effect), and a macro's
  arguments are auto-quoted for it. Builders never write a quote.
- **One type primitive, `mktype`**: `class`, `record`, `enum`, `verb`, `func`
  are prelude macros over `mktype` that push onto a compile-time builder
  stack, resolved by their `end<kind>` terminator, with StrongForth stack-
  effect checking of the def region for legible malformed-definition errors.
- **Expand-then-typecheck**: a macro emits ordinary core forms, and the
  checker runs on the expansion. A macro adds no type-checker vocabulary,
  which is the property that keeps it from escalating past the base layer.

## The macro-authoring surface

**A macro is a `macro name(params) ... endmacro` definition**, in the same
`end<kind>` family as `verb` and `record`, and itself a def keyword the
meta-stack resolves. It runs at compile time, while the *caller* is compiled;
there is no separate marker for that (Forth's uppercase `IMMEDIATE`), because
`macro` already means compile-time, and an all-caps marker would clash with a
surface that folds case everywhere else. Its `params` bind the auto-quoted
argument forms, and its result is spliced into the call site as core forms.

There are two tiers of the one form, a declarative common case and a
procedural power case, the split every macro system settles into:

**Template macros (the common case).** The body is a quasiquoted template of
forms with `${}` holes filled from the parameters. Pattern in, forms out, with
no compile-time computation. This is the approachable tier, the Scheme
`syntax-rules` / Rust `macro_rules!` flavor, and it is what a new surface
construct or a simple mixin needs:

    macro swap(a, b)
        quote seq(
            let(tmp, infer, ${a})
            set(${a}, ${b})
            set(${b}, tmp))
    endmacro

**Procedural macros (the power case).** The body is meta-layer code that runs
at compile time: it may inspect its argument forms, introspect a type
(records.md D8), branch, loop, and assemble output with `quote` / `quasi`
before emitting. This is the Lisp `defmacro` / Rust proc-macro tier (Forth's
compile-time word), for a macro that must *compute* its expansion, for example a
serializer that iterates a record's fields. A template macro is the
degenerate procedural macro whose body is a single quasiquote, so the two are
one mechanism.

**Reification is `quote` / `quasi` with `${}` holes**, which is the meta-layer
home quote.md reserved for it: the builder surface has no `quote`, but a macro
body uses `quote form` to reify a form as data and `${}` to splice a value or
sub-form into a template. This is the same quotation boundary the runtime
layer's strings and data literals use (sequences.md), one rule across both
layers.

## The deferred facilities land here

Each waits on this surface and now has a place; the detailed spelling of each
is its own follow-on, but the mechanism is fixed:

- **Mixins / traits** (shared-params.md). A mixin is **a macro that expands
  into a class body**, adding self-fields plus a sendable verb. The safety is
  structural: the macro's field accesses expand to `self`-access *in the
  including class*, which is legitimate intra-object code, and a macro that
  tried to emit `other.field = x` would produce a form the base layer rejects.
  So a mixin gives reuse, not new powers, and cross-object utility reaches the
  behavior only through the verb the class chose to include. The `include`
  spelling and member-merge rules are the mixin follow-on; the mechanism is a
  class-body macro.
- **Record introspection** (records.md D8). The field-presence boolean, the
  field-type check, and field iteration over (name, type) pairs are
  **meta-layer primitives a procedural macro calls** over a record type, all
  compile-time with zero runtime cost (the Zig / D / Nim model). They read the
  builder-stack record layout the meta layer already has. The record-
  introspection follow-on settles the primitive names.
- **Typed macros and the `_Generic` tier** (core.md, typed-data.md stage 4).
  A typed macro inspects an argument's *static type* and emits the matching
  monomorphic body (`max`, `abs`, list helpers), and typed holes (`${T}` in a
  shape or template) are the same idea in data. This is deferred until the
  `_Generic` tier is built; the first macro implementation is untyped
  expand-then-typecheck.

## Guardrails

The meta layer is powerful, so its walls are load-bearing:

- **Tier 3, library authors only** (tiers.md). Story and map builders never
  see the concatenative layer; the standard control forms (`if`, `for`,
  `class`, ...) are a universal prelude of macros a builder treats as
  keywords.
- **No escalation past the base layer.** A macro emits base-layer forms and
  the base checker validates them, so a macro cannot produce code the base
  language forbids (the mixin encapsulation argument above generalizes).
  Macros rearrange existing power; they do not mint new power.
- **Compile-time only.** Macros run at compile time and expand away; there is
  no runtime `eval` and no distribution of generic IR blobs (which would need
  a per-snippet runtime linker, rejected in core.md). Full runtime code
  construction stays a gated non-goal (the code-as-data spectrum, core.md).
- **Stack-effect checked, expand-then-typecheck.** Malformed definitions are
  stack-effect violations reported on the offending cell's `(file, line)`
  span; expansions are type-checked as ordinary core forms.

## Survey

- **Forth's immediate words**: the mechanism, a word that runs during
  compilation and shapes the output, over a compile-time stack. Excelsior's
  meta-stack is this, made statically checked, and without Forth's uppercase
  `IMMEDIATE` marker since every macro is compile-time by definition.
- **Lisp `defmacro` / Scheme `syntax-rules` + `syntax-case`**: the
  declarative-template versus procedural split adopted here, and homoiconicity
  (code is data via `quote`).
- **Rust `macro_rules!` versus proc-macros**: the same two tiers with a hard
  wall between the declarative common case and the procedural power case;
  Excelsior keeps one `macro` form spanning both.
- **Nim `template` versus `macro`**: template for the pattern splice, macro
  for AST computation, the exact two tiers, and Nim's compile-time field
  iteration is the introspection precedent.
- **Template Haskell / Julia macros**: staged compile-time metaprogramming
  with type information available, the typed-macro tier deferred here.

Excelsior's stance: Forth's compile-time mechanism, the Scheme / Rust / Nim
two-tier surface (template splice plus procedural computation) under one
`macro` form, stack-effect-checked and expand-then-typechecked, reification by
`quote` / `quasi`, and the whole layer walled off to library authors so the
builder surface stays small.

## Decisions (confirmed)

The six decisions are confirmed. The meta layer is a substantial build (a
compile-time macro expander over the form representation, the meta-stack with
stack-effect checking, and the two macro tiers); it is the platform for the
mixin, introspection, and typed-macro follow-ons, not a single implementation
step.


**D1. `meta.md` is the authoritative meta-layer surface**, consolidating
core.md's model (which stays the deep reference). The settled model, forms,
implicit quoting, `mktype`, the meta-stack, and expand-then-typecheck, is
carried forward unchanged.

**D2. A macro is a `macro name(params) ... endmacro` definition**, an
`end<kind>` def keyword resolved by the meta-stack, running at compile time
while the caller compiles (no separate immediate marker; `macro` means
compile-time), with auto-quoted argument forms, splicing core forms into the
call site.

**D3. Two tiers of the one form: template and procedural.** A template macro's
body is a quasiquoted form template with `${}` holes (the declarative common
case, Scheme `syntax-rules` flavor); a procedural macro's body is compile-time
meta-layer code that introspects, computes, and emits (the Lisp `defmacro` /
Rust proc-macro power case). A template is a procedural macro whose body is a
single quasiquote.

**D4. Reification is `quote` / `quasi` with `${}` holes, in the meta layer
only** (quote.md's reserved home). The builder surface has no `quote`; a macro
body reifies with `quote form` and splices with `${}`, the same quotation
boundary the runtime layer uses.

**D5. The deferred facilities are meta-layer facilities on this surface.**
Mixins are class-body macros (shared-params.md); record introspection is
meta-layer primitives a procedural macro calls (records.md D8); typed macros /
the `_Generic` tier and typed holes are deferred until that tier is built
(typed-data.md stage 4). Each facility's detailed spelling is its own
follow-on; this note fixes their mechanism and place.

**D6. The guardrails hold: tier 3, library authors only, no escalation past
the base layer, compile-time only, stack-effect checked.** A macro rearranges
existing power and emits base forms the base checker validates; it mints no
new power, reaches no runtime `eval`, and never appears on the builder
surface.
