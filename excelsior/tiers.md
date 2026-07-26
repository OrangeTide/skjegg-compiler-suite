# Tiers of the surface: what the tutorial teaches

Status: decided (2026-07), not yet implemented. The pass on
approachability.md's R12 (no defined beginner subset). The design names
three builder personas (core.md: map/object layers, story/dialog writers,
mechanics experts) but no document says which slice of the surface each is
expected to touch, so the grammar and keyword list present one flat language
to a non-programmer and an expert alike. This note defines three tiers,
sorts the surface into them, states what the tutorial teaches and what it
deliberately omits, and makes the tiering a forcing function: every future
feature declares its tier. The five decisions are confirmed; stamping the
back-catalog with tier lines and writing the tutorial outline are the
follow-up.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

## The problem

A flat surface is a hazard for the beginner audience specifically. A
non-programmer content creator opening the keyword list meets `quote`,
`can fail`, `xor`, and the macro layer with the same visual weight as `if`,
`tell`, and `${}`. Nothing marks the first group as "not for you yet", so
the language reads as larger and more forbidding than the slice a story
writer actually needs, which is the opposite of approachable.

The three personas already differ only in authoring surface (core.md), so
the slices exist implicitly. They have never been written down. Without the
map, three things go wrong: a tutorial has no principled line between what
to teach and what to defer; a beginner cannot tell which features they can
safely ignore; and every new feature is added flat, because there is no tier
for it to belong to. R12 asks for the map and for the discipline that keeps
it current.

## Three tiers

The tiers are depths of the one language, not three dialects. Every tier
compiles with the same compiler; a tier is a teaching and design-discipline
boundary, not a compiler-enforced restriction (see D5). They map onto the
personas:

**Tier 1, World.** The approachable core, serving the two non-programmer
personas (map/object builders and story/dialog writers). It has two facing
surfaces over one shared core: structure (rooms, objects, classes, verbs,
sends) for the map builder, and prose plus data (interpolated text, lists,
dialog data, match) for the story writer. This is the tutorial tier. It is
complete enough to build and ship a working room-and-dialog game without
touching a tier-2 feature.

**Tier 2, Mechanics.** The systems layer, serving the mechanics-expert
persona: the numeric and structural machinery a stats/combat/economy author
needs and a story writer does not. The tutorial names that these exist and
points at each with one line ("there is a tool for this when you need it"),
but does not teach them.

**Tier 3, Meta.** The language-shaping layer: the quotation boundary,
macros, the uniform core, and the future mixin/trait system. A mechanics
expert graduates here to write reusable machinery, and a framework author
lives here. It has its own advanced track and appears in no tutorial.

## The surface, sorted

Tiering the shipped and decided surface. The forcing function (D4) keeps
this current as features land.

**Tier 1, World (the tutorial teaches all of this):**

- values `int`, `decimal`, `str`, `bool`; `var` and `const`; literals and
  string interpolation `"...${e}..."`.
- lists: data literals `[a b c]`, 1-based indexing, `for x in xs`, `length` /
  `append` / `set` / `delete`, membership `x in xs` and `sub in s`, slices.
- control: the `if` statement and the `cond then A else B` if-expression;
  `match`; `while`; `for`; `break` / `continue`.
- consuming failure: `otherwise`, the inline `on fail` handler, and
  `if var` / `while var`. A beginner meets these the first time an index can
  miss, so consumption is tier 1 even though declaring a fallible producer
  is tier 2.
- objects: `class`, fields (`self.field`), `verb`, sends `recv.verb(args)`,
  `spawn`, `self`, `nil` / `nothing`.
- enums: declaring `enum Color [...]` and matching on members.
- output: `player.tell(...)`.
- `and` / `or` / `not`; a single comparison.

**Tier 2, Mechanics (named in the tutorial, taught in the mechanics guide):**

- `float` (IEEE double) and float/decimal/int casts. The tutorial's number
  story ends at "whole numbers are int, decimals are decimal"; float is the
  by-name ask for trigonometry-shaped math (numbers.md).
- declaring a fallible producer: `func ... can fail`, `returns maybe T`,
  capture into `maybe T`, `find`. (Consuming failure stays tier 1.)
- `shared` by-reference parameters (shared-params.md).
- `shape` declarations for typed data (typed-data.md). Writing a
  shape-typed data literal is tier 1; authoring the schema is tier 2.
- logical `xor`, chained comparisons (`a < b < c`), and any future bitwise
  family (`band` / `shl` and kin, not yet present). A non-programmer never
  reaches for these.
- deeper func composition and recursion.
- the authorship and powers surface: the `.exs` / `.exi` split,
  `discloses`, `deny`, the dangerous-power gates.

**Tier 3, Meta (its own advanced track, no tutorial):**

- `quote` and the quotation boundary; the uniform core of message/call
  forms.
- macros (the Forth IMMEDIATE-word mechanism) and compile-time evaluation.
- the future mixin / trait system (shared-params.md redirected cross-object
  reuse here).

## Remove, do not tier, when the feature was expert-only by accident

Tiering is not the only answer to "this is too advanced for a beginner",
and it is not the first. The numbers pass is the precedent: R3/R17 found the
binary `fixed` type and its `0f` / `fixed(num, den)` constructor were
expert-facing, and the fix was not to file them under tier 2, it was to
delete the whole apparatus and replace it with base-10 `decimal` that a
beginner reads at sight (numbers.md). Likewise R13 retired `select` rather
than tiering it (choosers.md). The rule: if a feature is expert-facing only
because it was designed awkwardly, fix or remove it; reserve a tier for
genuine depth (float, macros) that a beginner correctly does not need yet.
A large tier 2 is a warning sign, not a goal.

## The tutorial teaches tier 1 and names its omissions

The tutorial covers tier 1 completely and, at the point where a tier-2 need
would first arise, names the tool in one line without teaching it:

- reaching for angles or continuous math: "float is in the mechanics
  guide."
- writing a function that can fail for its own reasons: "declaring a
  fallible function is in the mechanics guide; here you only consume failure
  with `otherwise` and `on fail`."
- a helper that changes a caller's variable in place: "`shared` parameters
  are in the mechanics guide."

Each pointer is a door, not a wall: the beginner learns the feature exists
and where it lives, and is not asked to understand it to finish the
tutorial. This is the R12 deliverable, "what the tutorial teaches and what
it deliberately omits", made concrete.

## The forcing function

Every future design note names the feature's tier, in one line in its
header or its decisions. The discipline is what keeps the surface from
silently growing flat again: a feature that cannot name its tier is a
feature whose audience has not been decided, which is exactly the gap R12
found. This note holds the master table above; a new note that adds surface
updates it. Retroactively stamping the existing notes with a tier line, and
writing the actual tutorial outline, are the follow-up to this pass.

## Survey

Teaching languages that define a subset, and where the line is drawn:

- **Racket language levels** (DrRacket: Beginning Student, Intermediate,
  Advanced, Full Racket). The strongest precedent, and the one that goes
  furthest: the levels are compiler-enforced, so a construct outside the
  selected level is a syntax error. Excelsior borrows the graduated-surface
  idea but not (for v1) the enforcement, keeping one language rather than
  four dialects.
- **Inform 7**: a readable natural-language tier over an Inform 6 power tier
  reached through `include` blocks. A cited Excelsior precedent, and exactly
  the world-tier-over-mechanics-tier shape here.
- **Logo**: turtle-graphics tier over a full Lisp-like language underneath.
  Most learners never descend.
- **Excel**: the formula surface over the VBA surface. Two populations, one
  product, an explicit escape hatch between them.
- **HyperTalk / AppleScript**: a gentle English-like surface with a lower
  level available. The readable-for-the-layperson precedents Excelsior
  already cites.
- **C++**: no defined subset; every reader meets all of it. The anti-model,
  and the reason "you don't pay for what you don't use" is a runtime promise
  that does nothing for approachability.

Excelsior's stance: Racket's graduated surface and Inform 7's
readable-over-powerful split, delivered as documentation and design
discipline rather than compiler-enforced levels, so the language stays one
language a builder grows within.

## Decisions (confirmed)

**D1. Three tiers of the surface, mapped onto the three personas.** World
(tier 1, the two non-programmer personas, the tutorial tier), Mechanics
(tier 2, the mechanics-expert persona), Meta (tier 3, framework authors).
Depths of one language, not three dialects.

**D2. The tutorial teaches tier 1 completely and names its omissions.** At
each point a tier-2 need first arises, it names the tool in one line and
sends the reader to the mechanics guide, without teaching it. Tier 1 is
complete enough to ship a working game.

**D3. The shipped and decided surface is sorted by the master table above.**
Consuming failure (`otherwise` / `on fail` / `if var`) is tier 1; declaring
a fallible producer is tier 2. Writing a shape-typed literal is tier 1;
authoring the schema is tier 2. `float`, `shared`, `xor`, chained
comparisons, bitwise, and the powers surface are tier 2. `quote`, macros,
the uniform core, and mixins are tier 3.

**D4. Every future design note names the feature's tier.** A one-line tier
declaration in the note, and an update to the master table when a note adds
surface. A feature that cannot name its tier has not decided its audience.

**D5. Tiers are documentation and design discipline, not compiler-enforced
levels, for v1.** Racket-style enforced levels and a module-level tier cap
(reusing the `deny` mechanism) are recorded as a future option and deferred.
One compiler, one language, three teaching depths.
