# Enums that work: small-int members, name labels, name printing

Status: decided (2026-07); implemented (D1-D6). The pass on
approachability.md's R11 (enum sequencing dead-ends). Enums parse in the
showcase word-list syntax and resolve, but an enum value did not lower
and enum members could not label a `match`, so the construct most likely to
appear in a beginner's first class (item kinds, directions) stopped after it
was declared. This note settled the representation and the semantics so it
became a working, first-class value. The core (D1-D6) is implemented: a member
lowers to its 0-based ordinal, qualified in value positions and bare as a match
label, the enum match is checked exhaustive, and a hole prints the member name
via a per-enum name table. The D7 friendliness and power adds remain deferred.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

## What exists today

- **Declaration** parses: `enum Color [red green blue]`, and the bracket
  carries a large enum across lines (grammar.ebnf). It produces `N_ENUM`
  with a member per word.
- **Resolution** defines the enum as `SYM_ENUM` at module scope and each
  member as `SYM_ENUM_MEMBER` in the enum's member scope, so `Color.red`
  resolves as a qualified member access.
- **Type checking** types `Color.red` as the enum type `Color`
  (`check_member_value` returns the enum's named type); a bare `red` in a
  value position is "not a value".
- **Lowering** does nothing with an enum member (it is on the unimplemented
  list), so a program that uses `Color.red` as a value has no runtime
  representation.
- **Match** labels must be constants or literals; an enum member is
  neither, so `match dir` cannot name members in its arms.

No test uses an enum as a value; they only declare. The gap is entirely
below the surface.

## The representation

**An enum member is a small int, its 0-based position in the declaration
order.** `Color.red` is 0, `Color.green` is 1, `Color.blue` is 2. Enum
values are word-sized and reuse int storage exactly as bool and decimal
do, so fields, locals, parameters, and `list<Direction>` all work with no
new storage machinery, comparison is an int compare, and a match dispatch
is the int compare or jump table it already lowers to. This is R11's
"lower enums as small ints", and it is what makes the rest cheap.

The member's ordinal is fixed by its position in the declaration, so
reordering the word list renumbers the members. That is the same contract
Pascal and C enums have, and it matters only if an enum value is ever
persisted (freeze/thaw), where the stable key is the member name, not the
ordinal; that is a persistence concern for host-abi, not for the value
semantics here.

## Naming a member: qualified in values, bare in labels

Two positions, one rule each, drawn along the value-versus-pattern line:

- **In a value position** (an assignment, an argument, a field default, a
  comparison), a member is written **qualified**: `Direction.north`. This
  is what resolves today, it is unambiguous, and it never collides with a
  local or global of the same spelling. `var heading is Direction =
  Direction.north`.
- **As a `match` arm label**, a member is written **bare**: `north`. A
  label is a pattern, not a value, so a bare name there cannot be mistaken
  for a variable read, and the subject pins which enum's members are in
  scope. This is R11's "match labels name members", and it is where enums
  earn their keep:

      match heading
          north then go(0, 1)
          south then go(0, -1)
          east  then go(1, 0)
          west  then go(-1, 0)
          up, down then stay()      // a member list, like any match arm
      endmatch

The teaching line is one sentence: "in a `match`, the arms name the
members of what you are matching; elsewhere, name a member with its enum."
Contextual bare names in value positions (Swift's leading-dot `.north`, or
resolving a bare name against a declared enum type) are a friendlier but
ambiguity-prone extension, deferred (see the end).

## Match is exhaustive for enums

Because an enum has a known, finite set of members, a `match` on an enum
subject **must cover every member or carry an `otherwise`**, or it is a
compile error that names the missing members:

    match heading                    // error: does not handle east, west, up, down
        north then ...
        south then ...
    endmatch

This is the teaching win of the pass. Today a statement `match` with no
matching arm is a silent no-op and an expression `match` traps at
runtime; for a beginner who forgot the `west` case, both are bugs found
late. Exhaustiveness moves the forgotten case to a compile-time message
that lists exactly what is unhandled, and "handle the rest" is a single
`otherwise`. It applies only to enum subjects, where the member set is
known; int and decimal subjects keep their current open semantics.

## Enums are opaque labels

An enum is a set of names, not a set of numbers. So:

- **Equality only.** `==` and `!=` compare two values of the *same* enum;
  comparing a `Color` to a `Direction`, or to an int, is a type error.
- **No ordering.** `Color.red < Color.blue` is rejected. Directions and
  item-kinds have no meaningful "less than", and hiding one behind the
  declaration order invites nonsense. An enum that genuinely needs ordered
  levels is a later, opt-in consideration, not the default.
- **No arithmetic and no implicit int.** `Color.red + 1` and an implicit
  `Color` to int are rejected, so a beginner cannot fall out of the set of
  names into raw arithmetic. An explicit `as int` for advanced use is
  deferred.

The int representation is an implementation fact the surface does not
expose; a `Color` behaves as one of three names, nothing more.

## Printing shows the name

Interpolating an enum yields the member's **word**, not its ordinal:

    heading = Direction.north
    player.tell("You head ${heading}.")     // "You head north."

The compiler emits a per-enum table of the member spellings in ordinal
order, and enum interpolation (and any future `tostr`) routes through an
`__exc_str_from_enum(table, value)` helper that indexes it. This is a
first-class need for the audience: an enum that printed as `0` would be
useless in game text. It is also why the member names must survive to
runtime for the enums that are printed.

A **two-member enum is the declare-once bool word pair** (`enum Lockness
[locked unlocked]`, `enum Wetness [wet dry]`): the same name-printing gives
`"${state}"` the word `locked` in a string hole, which is the reusable form
of the inline `cond then A else B` pick. It needs nothing beyond D6; it is
this section applied to a two-member set.

## Survey

- **Pascal**: ordered, int-backed, `for d := Low to High`, `Ord`/`Succ`.
  Ordering and iteration are first-class; Excelsior keeps the int backing
  and the finite set but drops ordering by default as an opaqueness
  choice.
- **C**: a thin alias for int, no type safety, leaks into arithmetic
  freely. The anti-model for the opacity decision here.
- **Rust**: opaque, no implicit int, `match` is exhaustive, the name
  prints only via a derived trait. This is closest to the stance here.
- **Swift**: opaque, exhaustive `switch`, leading-dot `.north` in
  known-type contexts, `CaseIterable` for iteration, `rawValue` for the
  explicit int. The leading-dot is the deferred value-position nicety.
- **Ada**: ordered, `'Image` gives the name, `'Val`/`'Pos` convert. The
  name-printing is the `'Image` idea.

Excelsior's stance: opaque labels (Rust/Swift), small-int representation
(all), bare labels in the pattern position (the match subject pins the
enum), name printing (Ada `'Image`, Swift), and exhaustive enum matches
(Rust/Swift) as the beginner safety net.

## Decisions (confirmed)

**D1. An enum member is a small int, its 0-based declaration position.**
Word-sized, reusing int storage, so fields, locals, params, lists,
comparison, and match dispatch all reuse the int machinery. This is the
whole of "lower enums as small ints".

**D2. Members are qualified in value positions (`Direction.north`) and
bare as match labels (`north`).** The split is value versus pattern: a
label is a pattern the subject pins, so a bare name is unambiguous there,
while a value position keeps qualification to avoid colliding with a
local.

**D3. A `match` on an enum subject uses bare member labels and member
lists; the default is `otherwise`.** The arms name the subject enum's
members.

**D4. An enum `match` must be exhaustive: cover every member or carry an
`otherwise`, else a compile error listing the missing members.** The
teaching win, turning a forgotten case from a late bug into a compile-time
message. Enum subjects only; int/decimal matches keep open semantics.

**D5. Enums are opaque labels.** `==`/`!=` within one enum, no cross-enum
or enum-to-int comparison, no ordering, no arithmetic, no implicit int
conversion.

**D6. Printing yields the member name.** An emitted per-enum name table
and an `__exc_str_from_enum` helper give `"${heading}"` the word
`north`, not `0`.

**D7. Deferred.** Contextual bare member names in value positions (a
leading-dot or type-directed lookup), `for d in Direction` iteration over
all members, ordered enums for the cases that want them, an explicit `as
int`, and `maybe Direction`. Each is a friendliness or power add on top of
the working core, not part of closing the R11 dead-end.
