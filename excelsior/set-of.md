# Flag sets: `set of` a small element universe

Status: decided (2026-07); core implemented. The single-word case (universe <=
32 members) works: an inline `set of E` type, bare/qualified members, `empty` /
`full`, the `+` / `-` / `^` operators, and the `in` / `overlaps` tests, over
vars, fields, params, and sends. Deferred: the named alias `Name is set of E`
(D3), a universe over 32 members (the fixed blob), `char` sets (D2), a
constructed set as a direct call argument (build into a var first), set
equality and a set `match` subject (D7), and richer set field defaults (only
`empty`). A standalone pass from the backlog (backlog.md, TODO's
`set of` research item), independent of the data-model spine. Tier: using flags
is World (a content creator toggles a door's or an entity's flags with `+` /
`-` / `in`), declaring a `set of` type and its enum is Mechanics (tiers.md).
Depends on enums.md (the member universe) and reuses its contextual bare-member
mechanism, so it follows enums in implementation.

A game carries many boolean flags: an entity's damage resistances, a door's
`locked` / `trapped` / `hidden` state, a status effect stack. Storing each as a
separate `bool` field is verbose and unstructured, and raw bitmask integers are
exactly the unpredictable bit-twiddling boolean-ops.md kept off the surface. A
**set** stores a group of flags compactly (one bit per possible member) while
exposing them as first-class **named** flags, not bit positions. This is
Pascal's `set of`, brought forward for the audience.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

## What a set is

**`set of E` is a value type: a fixed-size bitmask over a small element
universe `E`, with one bit per possible member of `E`.** It has value
semantics, exactly like a record (records.md): assigning a set copies its bits,
passing one copies it, and there is no identity. A set is data you hold and
copy, and its whole value is which members are present.

The size is fixed at compile time to `E`'s cardinality: a small enum's set fits
in a single word, and the universe is capped at **256 members (32 bytes)**, so
a set is a small fixed blob, one word for the common enum case and a few words
at most. A universe larger than 256 is a compile error (which is why `set of
int` is rejected, an int's universe is billions; you set over an enum, a
`char`, or a small ordinal, not a wide scalar).

## The element universe: `set of Enum`, and `set of char`

The element `E` is a **small, closed universe**, and the two element kinds are:

- **an enum** (enums.md), the main case. An enum is the "one-of" (a single
  value: one `Direction`, one `Element`), and `set of Element` is the "many-of"
  (any subset of the elements). The enum supplies the member names, and the set
  reuses them, so there is one place members are declared.
- **`char`** (256 code points in the v1 byte view, text-encoding.md), the
  Pascal `set of Char` classic, for character-class tests (`c in whitespace`).

A dedicated `flags` declaration (a `[Flags]`-style type separate from a one-of
enum) was the alternative. It is declined: `set of Enum` reuses the enum member
list rather than duplicating it, keeps one member-declaration concept, and
matches Pascal, where the enum-versus-`set of`-enum pairing is exactly the
one-of-versus-many-of distinction a builder already needs. Small integer
subranges as an element type (Pascal's `set of 0..31`) wait on a subrange type
the language does not yet have; enum and `char` are the v1 universes.

## Named flag types, and bare members

A flag type is normally **named**, because the same set of flags is reused
across many fields:

    type
        enum Element [fire frost poison arcane]
        EffectsFlags is set of Element

    var resist is EffectsFlags = fire + frost

`EffectsFlags is set of Element` binds a readable name to the set type, sitting
in the `type` section beside the `enum` and `class` declarations. It is the
first concrete instance of the named type alias union-types.md deferred; the
exact declaration keyword or placement is a small surface sub-decision to
settle in implementation (as records.md left its field separator), but the
`Name is set of E` shape is fixed. Inline `set of Element` without a name also
works (`var s is set of Element`), for a one-off.

**Members appear bare in a set context.** enums.md qualifies an enum member as
`Element.fire` in a value position and allows it bare only where the subject
pins the enum (a match label). A set construction or test **pins the element
enum the same way**: in `resist = fire + frost`, the target type `EffectsFlags`
fixes the element enum, so `fire` and `frost` are bare singleton sets; in `if
fire in resist`, `resist`'s type pins it. This pass activates enums.md's
deferred contextual bare names for set-pinned positions, which is what lets a
flag read as a plain word (`fire + poison + sleep`) rather than
`Element.fire + Element.poison + ...`. Where no set context pins the enum, a
member is written `Element.fire` as usual.

## Building and changing a set: symbols for values, words for tests

The design splits cleanly along "symbols do math, words do structure": the
**value algebra is symbols** (`+` `-` `^`, building and changing a set value),
and the **boolean questions are words** (`in`, `overlaps`, reading as English).

**Each flag is itself a singleton set** (a bitmask with one bit set), so every
operator is a uniform set-to-set operation with no member-versus-set coercion
to reason about:

- **`+` / `+=` union** adds a flag or another set: `resist = fire + frost`,
  `effects += confusion`.
- **`-` / `-=` difference** removes: `flags -= hidden`, `defaultFlags - living`.
- **`^` / `^=` symmetric difference / toggle** flips membership: `s ^= trapped`
  turns `trapped` on if absent and off if present, and `^` on two sets is their
  symmetric difference (the set delta).

Because `+` and `-` build a set, there is **no constructor word and no
delimiter**: a set value is written by unioning flags, and `+` is the
universally understood plus, not a novel sigil a newcomer must look up (the
searchability concern behind words-over-symbols; a magic bracket like `(: :)`
is exactly what this avoids, and it does not collide with the `[...]` data
literal). The empty and full sets, which `+`-over-members cannot spell, are the
context-typed literals **`empty`** and **`full`**: `var s is EffectsFlags =
empty`, `resist = full`. Each is a bare reserved word typed by its context, and
each is distinct from `nothing` (nil-nothing.md): an `empty` set is a present
set holding no members, not the absence of a set.

A worked example, deriving one set from another as plain left-to-right algebra:

    const defaultFlags is EntityFlags = living + movable + animated + breathing + mortal
    var   vampireFlags is EntityFlags = defaultFlags - living - mortal + undead

Every operand is a set, the multi-flag `defaultFlags` and the singleton flags
(`living`, `undead`) alike, so a derived set reads as arithmetic on flags.

## The two membership tests: `in` and `overlaps`

Two boolean questions arise about a set, and each gets a readable word:

- **`in` is containment (all-present).** `fire in resist` asks whether the flag
  is present, and because a flag is a singleton set, `in` is set containment
  (subset): `fire + frost in resist` asks whether **both** are present (the
  left set is a subset of the right). This is the all-present, AND-over-flags
  test.
- **`overlaps` is non-empty intersection (any-present).** `resist overlaps
  (fire + frost)` asks whether **any** of the flags is present (the two sets
  share at least one member). This is the complementary any-present,
  OR-over-flags test, and it settles the question the backlog left open. The
  neophyte can always spell it out as `fire in resist or frost in resist`;
  `overlaps` is the compact form for many flags, chosen as a plain English word
  over Pascal's `s * (fire + frost) != empty` (set multiplication a beginner
  cannot read) and over the terser `intersects`.

`in` **reconciles with its list and string use** rather than conflicting:
`in` asks "is the left contained in the right" everywhere. For a list or string
the left is an element contained as a member (`x in xs`, `sub in s`); for a set
the left is itself a set contained as a subset, and since a single flag is a
singleton set, the one-flag case `fire in resist` reads exactly like element
membership, so a builder meets no seam. Precedence follows the split: the value
operators `+ - ^` bind at the additive level (left to right), `in` and
`overlaps` are comparisons below them (so `fire + frost in resist` groups as
`(fire + frost) in resist`), and the boolean words `and` / `or` sit lowest (so
`fire in resist or frost in resist` groups correctly with no parens).

A set intersection as a *value* (Pascal's `*`, "which of these are present")
is not needed for the flags-as-booleans use `overlaps` covers, so it is left
out of v1; if a use appears it is the Pascal `*`, completing the `+` `-` `*`
`^` algebra.

## A set is a value: equality, match, and sends

A set has **value equality**: two sets are equal when they hold the same
members (`resist == full`, `flags == empty`). So a set is usable as a **match
subject** with constant set labels, dispatching on an exact combination:

    match doorFlags
        empty            then open()
        locked           then rattle()
        locked + trapped then springTrap()
        otherwise             inspect()
    endmatch

The labels are set constants compared by equality, exactly as an int or enum
match works. This is dispatch on the *whole* set value; the "is this one flag
present" question is not a match (a set with several members is not one label),
it is an `in` / `overlaps` test in an `if` or a `cond then A else B`. So the
backlog's "set as a match subject" is equality on set constants, and membership
dispatch stays with the tests.

Because a set is a fixed-size value, it **crosses a send by copy**, like a
record (records.md): a one-word small-enum set marshals as a word, a larger set
as its fixed blob of data, with no shared identity. A set is a natural verb
argument ("apply these status effects") and return value.

## Representation and memory

A `set of E` is a fixed bitmask of `ceil(cardinality(E) / bits-per-word)`
words, sized at compile time. A small enum (up to a word of members) is one
word; `set of char` is 256 bits (32 bytes). Membership and mutation lower to
word-indexed bit operations (test bit, set bit, clear bit, xor bit), and the
set operators lower to word-wise `and` / `or` / `andnot` / `xor` over the blob.
There is no allocation and no identity: a set lives inline in its container (a
local, a field, a list element), is copied by value, and is reclaimed with its
container, the same inline no-GC story as a record. This is why the universe is
capped at 256, the blob stays small and stack-friendly.

## Survey

- **Pascal / Object Pascal `set of`**: the direct ancestor, `set of TColor`,
  `[red, blue]` literals, `+` `-` `*` operators, `in` membership, sets in
  `case`. Excelsior keeps the type and the `+`/`-`/`in` core, replaces the
  `[...]` literal with `+`-union (no bracket collision), replaces `*`-plus-test
  with the word `overlaps`, and takes `^` for toggle over Pascal's `><`.
- **C enum bitflags (`1 << N`, `|`, `&`)**: the raw bitmask this replaces; the
  named-flag surface hides the bit positions and the precedence traps.
- **C# `[Flags]` enum with `HasFlag`**: named combinable flags with `|` / `&`;
  `HasFlag` is `in`, `[Flags]` is the `set of Enum` pairing. The dedicated-
  attribute model this pass reads as "just set of the enum".
- **Python `set` / `frozenset`**: `|` `-` `^` `&`, `in`, and `^` for symmetric
  difference, the toggle precedent. Excelsior's fixed-universe set is the
  compile-time-sized cousin.
- **Rust `bitflags` crate**: named flags over an integer with `insert` /
  `remove` / `toggle` / `contains`, the same operations under method names;
  Excelsior spells them as operators and words.
- **MUD / MUSH flags**: PennMUSH / TinyMUSH set a flag by naming it (`@set obj
  = dark`) and clear it with a `!` prefix (`@set obj = !dark`), which maps onto
  `+` (set) and `-` (clear); the builder tradition treats add/remove as
  primary and has no toggle, which is why toggle takes the less-common `^`.

Excelsior's stance: Pascal `set of` an enum or `char`, value-semantic and
compile-time-sized, with a `+` / `-` / `^` value algebra (no constructor word
or bracket), `empty` / `full` identity literals, and `in` (all-present) plus
`overlaps` (any-present) as the two readable boolean tests.

## Decisions (confirmed)

The eight decisions are confirmed. The implementation (the compile-time-sized
bitmask layout, the `+`/`-`/`^` word-wise lowering, `in`/`overlaps` bit tests,
the `empty`/`full` literals, and the set-pinned contextual bare members) is the
follow-up, and depends on enums (enums.md) for the member universe and its
contextual bare names.

**D1. `set of E` is a compile-time-sized bitmask value type over a small
universe `E`**, with value semantics (copied like a record, inline, no GC), the
universe capped at 256 members (32 bytes) or a compile error.

**D2. The universe is an enum (the main case) or `char`; it is `set of
Enum`, not a parallel `flags` declaration.** The enum is the one-of and `set
of Enum` the many-of, reusing the enum's member list; integer-subrange element
types wait on a subrange type. `char` is the Pascal `set of Char` case.

**D3. A flag type is normally named (`EffectsFlags is set of Element` in the
`type` section); inline `set of E` also works.** This is the first instance of
union-types.md's deferred named type alias; the exact declaration keyword or
placement is a surface sub-decision, the `Name is set of E` shape is fixed.

**D4. Members are bare singleton sets in set-pinned contexts.** A set
construction or test pins the element enum the way a match subject does, so
`fire + poison` and `fire in resist` write members bare; elsewhere a member is
`Element.fire`. This activates enums.md's deferred contextual bare names for
set positions.

**D5. Value-building is symbols, boolean tests are words.** `+` / `+=` union,
`-` / `-=` difference, `^` / `^=` symmetric-difference/toggle build a set value
(each flag a singleton set, so all are set-to-set); there is no constructor
word or delimiter. `empty` and `full` are the context-typed identity literals,
distinct from `nothing`.

**D6. The two membership tests are `in` (all-present, subset/AND) and
`overlaps` (any-present, non-empty intersection/OR).** `overlaps` settles the
backlog's open any-present question as a plain word over `s * t != empty`. `in`
is uniform containment across lists, strings, and sets (a single flag is a
singleton set, so one-flag `in` reads as element membership). Precedence: `+ -
^` additive, `in` / `overlaps` comparisons, `and` / `or` lowest.

**D7. A set is a value with `==`, so it is a match subject with set-constant
labels (equality on exact combinations); membership dispatch is `in` /
`overlaps`, not match.** A set crosses a send by copy, like a record: a word
for a small enum, a fixed blob otherwise.

**D8. Toggle is `^` / `^=` (symmetric difference).** From the survey: the
near-universal XOR-toggle idiom (C / C++ / C# / Java / JS bit toggle, Python
set `^`), over Pascal's obscure `><`. MUD/MUSH set-by-name and clear-by-`!` map
to `+` and `-`, confirming add/remove as primary and toggle as the
less-common `^`.
