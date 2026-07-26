# Visibility stated once: per-member, not per-section

Status: decided (2026-07), not yet implemented. The pass on
approachability.md's R15 (member visibility is stated twice). Tier: World
(tier 1); this is the class surface every builder meets (tiers.md). The five
decisions are confirmed; the implementation (dropping the section state in
`parse_class`, the per-field prefix, and the migration hint) is the
follow-up.

A class body groups its members under `public` / `private` section headers
(Delphi-style), and every member obeys its section. But a `verb` is legal
only under `public` and a `func` only under `private`, so for callables the
section carries no information the keyword did not already fix. The builder
writes the fact twice and earns a teaching error when the two copies
disagree. This note removes the redundancy by making visibility a per-member
property stated exactly once, and retires the section headers.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

## What exists today

The class body is a sequence of `public` / `private` sections; each member
picks up the current section's visibility (parse.c `parse_class`, tracked in
`vis`). The parser enforces three rules:

- a **field** must sit inside a section (either), and its section decides
  whether its signature is exported to the generated `.exi` contract
  (core.md, the `.exs` / `.exi` split): `public` fields and `tunable` fields
  are in the contract, `private` fields are not.
- a **verb** must sit under `public`. A verb is an entry-point message, a
  dispatched descriptor slot, and a line in the `.exi`; a private verb is a
  contradiction in terms, since a send would still reach it.
- a **func** must sit under `private`. A func is a plain static helper that
  never enters the `.exi`.

So visibility is real and load-bearing for **fields** (it is the export
switch, and both values are legitimate), and fully **determined** for verbs
and funcs (the keyword pins it). The section header in front of a verb or a
func repeats what the keyword already said, and the only thing the
repetition can add is a disagreement, which is the teaching error R15 calls
good but unnecessary.

## The design

**Visibility is a per-member property, written once. The `public` /
`private` section headers retire.**

- **A verb is inherently public; a func is inherently private.** The
  keyword *is* the visibility. No visibility word is written on a verb or a
  func, and none is accepted. This is unchanged semantics, just without the
  redundant header, and it keeps the one-sentence rule intact: "public verbs
  are what others can ask you to do, private funcs are how you do it; the dot
  asks."

- **A field is private by default and takes an optional `public` prefix to
  export it.** A bare `hp is int = 10` is private (encapsulated state, not in
  the `.exi`). `public name is str` exports the signature to the contract. An
  explicit `private hp is int` is allowed and means the default, for a
  builder who wants to say it out loud. `tunable` stays an orthogonal prefix
  that opts a field into live-default propagation and, by necessity, into the
  contract; the prefixes stack as `[ public | private ] [ tunable ] name is
  type`.

A class then reads with every member self-describing:

    class BarrowChest is Container
        locked       is bool = true      // private: bare field
        opened_count is int  = 0         // private
        public name  is str              // exported to the .exi contract

        verb open(opener is obj)         // public: it is a verb
            ...
        endverb

        func reward(opener is obj)       // private: it is a func
            ...
        endfunc
    endclass

There is no section state to track, no place to state visibility twice, and
no way for two statements of it to disagree. The contradiction errors
(`a verb belongs under public`, `a func belongs under private`) retire with
the sections, because the situation they caught can no longer be written.

### Private by default is the right default

Fields default to private because encapsulated state is the safe teaching
stance and the common case: most fields are internal, and the exported ones
are the exception a builder deliberately opts into with `public`. This is
Rust's `pub` model (private unless marked), and it also means *less* writing
than a section header for the ordinary state-heavy class, where every field
would have sat under one `private` header anyway. The rare exported field
pays one word, `public`, exactly where it is declared.

### What "public" on a field means is unchanged

A `public` field is exported to the `.exi` contract, for the tooling,
persistence, and editor layer that reads the contract. It is **not** a
cross-object read: another script still cannot reach `goblin.health`
directly, because cross-object field access is not in the actor model
(verbs.md, shared-params.md); state a caller needs is offered through a
verb. This note does not change that boundary; it only changes where and how
often a field's export status is written. The potential confusion (a
beginner reading `public health` as "others can read it") is real but
pre-existing and orthogonal to R15; it belongs to a verbs/encapsulation
teaching pass, not here.

### Visibility is the export axis, not the inheritance axis

A fair question, since `public` / `private` were introduced into the design
early: does the distinction govern **inheritance**, so that a `private`
member is hidden from a subclass the way C++ and Java hide it (with
`protected` as the subclass-visible middle)? Today it does not, by design on
both halves:

- **For callables**, verbs.md already settled this: the model has *no
  protected override hooks*. A verb is public and sendable, a func is
  private, and a subclass-visible-but-unsendable callable is deliberately
  absent. If that pattern earns its way in, verbs.md commits it to "an
  explicit annotation rather than a quadrant accident", a separate concept,
  not a reuse of this switch.
- **For fields**, inheritance visibility is currently flat: the resolver
  chains a subclass's member scope onto its parent's (resolve.c), so a
  child's own `self.field` resolves an inherited field with no visibility
  filter. A `private` parent field is already reachable from the child's own
  methods. `public` / `private` decides `.exi` export, not what a descendant
  can see.

So visibility and inheritance-visibility are two axes, and this note only
touches the first. Retiring the section does not foreclose the second: if a
subclass-only or subclass-hidden distinction is ever wanted, it is a third
word (a `protected`-style annotation) added deliberately, exactly as
verbs.md already frames the protected-callable case, not a meaning smuggled
back into `public` / `private`. Keeping the export axis to one word per
member is what leaves that future axis a clean place to land.

### Migration

`public` and `private` become member-prefix modifiers on fields, not section
headers. A bare `public` or `private` on its own line (the old section
header) gets a teaching error with the migration: write the visibility on the
field itself, and drop it from verbs and funcs entirely. This is the same
migration-hint courtesy the other retired spellings get.

## Survey

Where languages put the visibility word:

- **C++ / Delphi / Object Pascal**: `public:` / `private:` section labels,
  the model here today. Economical when a section holds many members, but it
  makes visibility a mode the reader must track upward, and it invites the
  redundancy R15 found when a member kind already implies its visibility.
- **Java, C#**: a modifier on every member. Self-describing but verbose,
  with no default to lean on.
- **Rust**: private by default, `pub` opt-in. The model adopted here for
  fields: the safe default is free, the exception costs one word.
- **Swift, Kotlin**: per-member modifiers with a sensible default
  (`internal`), so most members are unmarked. Same shape as the field rule
  here.
- **Go**: visibility by the identifier's capitalization, no keyword at all.
  Maximally terse, but spelling-as-semantics does not suit a word-first,
  case-insensitive language.
- **Eiffel**: `feature { CLIENT }` selective export lists, the most precise
  and the heaviest. More than this audience needs.

Excelsior's stance folds visibility into the member keyword where the kind
fixes it (verb public, func private) and uses Rust's private-by-default with
a `public` opt-in where the field genuinely chooses, so visibility is stated
once per member and never twice.

## Decisions (confirmed)

**D1. Visibility is a per-member property, stated once; the `public` /
`private` section headers retire.** No section state, no second place to
state visibility, no way for two statements to disagree.

**D2. A verb is inherently public and a func inherently private; the keyword
is the visibility.** No visibility word is written or accepted on a callable.
The "public verbs / private funcs / the dot asks" teaching line is unchanged.

**D3. A field is private by default, with an optional `public` prefix to
export it to the `.exi` contract** (and an optional explicit `private` that
means the default). `tunable` stays an orthogonal, stacking prefix that
implies contract presence. Prefix order: `[ public | private ] [ tunable ]
name is type`.

**D4. The verb-under-private and func-under-public teaching errors retire
with the sections; a bare `public` / `private` section header gets a
migration hint** pointing at the per-member form.

**D5. Visibility is the export axis, not a cross-object read and not the
inheritance axis.** `public` on a field means export to the `.exi` contract
for the tooling and persistence layer; the actor-model boundary (foreign
state is reached through a verb) stands. Inheritance visibility is a separate
axis, currently flat (a class sees all its ancestors' members); a
`protected`-style restriction, if ever wanted, is a deliberate third word,
as verbs.md already frames the protected-callable case, not a reuse of
`public` / `private`. This note relocates only where export status is
declared.

## Clarification: inheritance and privacy (2026-07)

D5 already settles the question "can a subclass reach an inherited private
member" with "yes, inheritance visibility is flat". Asked again while
interface-support.md was being written, it is worth recording why that is
right rather than merely current, and what the tree actually does.

**There is no private verb, so the question is about funcs.** D2 makes a verb
inherently public and a func inherently private, so a private callable is a
`func` and the question is whether a subclass may call an inherited one. It
may, and it may read an inherited private field.

**`private` marks the contract, not an encapsulation boundary.** It means not
sendable and not in the `.exi`. There is no cross-object access to a field or
a func under any visibility; only verbs cross. So privacy is already total in
the direction that matters, and it is not doing a second job.

**A subclass is not another object.** It shares the field segment as a parent
prefix (host-abi.md) and the same `self`, so an inherited helper crosses no
boundary the word is about. Java needs `protected` because it has a third
audience between its `private` and its `public`; Excelsior has two, the
contract and the implementation, and a subclass is on the implementation side
by construction. A third word would be inventing the audience in order to
restrict it.

**Shadowing is well-behaved, which is the case that would otherwise bite.** A
subclass may declare a func with the same name as a parent's private one, and
each class's code calls its own, because a func is a direct call resolved at
its class rather than a dispatched send. So the fragile-base-class hazard
(a subclass silently capturing a parent's internal call) does not arise; only
a subclass that *names* an inherited helper depends on it, and that dependency
is visible in its source.

**State of the tree.** `NF_PUBLIC` is read in three places: the parser, to
enforce verb-under-public and func-under-private; the symbol dump; and
lowering, to choose static linkage. Nothing in the checker enforces visibility
on access, so D5's "flat" is currently true by absence rather than by rule.
When this note is implemented, that stays deliberate: the per-member prefix
decides `.exi` export, and no access check is added.
