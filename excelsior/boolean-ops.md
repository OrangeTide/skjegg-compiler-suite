# Boolean operators: `and`, `or`, `not`, and retiring `xor`

Status: decided (2026-07), not yet implemented. The pass on
approachability.md's R18 (`xor` as a core word operator). Tier: World (tier
1); the boolean operators are core surface every builder meets (tiers.md).
The three decisions are confirmed; the implementation (removing the `xor`
keyword and its `parse_or_level` arm, the migration hint) is the follow-up,
and the bitwise intrinsics are a recorded future direction, not shipped.

Bool `xor` is `!=` on bools. The audience will never reach for it, and it
occupies a keyword and a precedence slot for a meaning an existing operator
already carries. This note retires it, leaving `and` / `or` / `not` as the
boolean set, which is exactly the trio the beginner precedents ship.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

## What exists today

`xor` is a keyword (T_XOR) parsed at the `or` precedence tier (parse.c
`parse_or_level` accepts `or` and `xor` together), type-checked as bool-only
(typecheck.c requires bool operands and returns bool), and lowered
(lower.c). It is purely logical exclusive-or on two bools; there is no
bitwise `xor`, and no other feature depends on it. So for any two bools,
`a xor b` and `a != b` compute the same value.

The two are not textually interchangeable only because of precedence: `xor`
binds low (the `or` tier), while `!=` binds at the equality tier, so a larger
expression parenthesizes differently. That precedence slot is part of what
`xor` costs, not a capability it adds: the rare author who wants a
low-binding exclusive-or writes `(a) != (b)`.

## The design

**Retire `xor`. The boolean operators are `and`, `or`, `not`.**

`a != b` is the exclusive-or of two bools, and it reads as what it is: "these
two differ", which for bools is "exactly one is true". A beginner never
writes `xor`; the boolean vocabulary they need is the three words that read
as English (`and`, `or`, `not`), and every beginner precedent ships exactly
those three. Keeping a fourth word that duplicates `!=` adds a keyword to the
list a builder scans, a precedence tier to the grammar, and a "which do I
use, `xor` or `!=`?" question, all for zero new capability.

This is tiers.md's remove-don't-tier rule applied cleanly. `xor` is not depth
a beginner reaches later (like float or macros); it is redundant surface, so
the answer is to fold it into `!=`, not to file it under the mechanics tier.
It is the same call R13 made on `select` and R16 made on `trace`: when the
weaker form is subsumed by one that already ships, it goes. The contrast with
`///`, kept in the R16 pass because it was genuine depth once promoted, is
what the rule is for: it removes duplication and keeps capability.

R18 points at the reasoning inside the language: bitwise operators are meant
to stay out of the core operator space, precisely because a beginner does not
reach for them. Word `xor` belongs with that reasoning. But R18's own framing
(a `band` / `bor` infix word family) keeps the deeper hazard it was trying to
avoid: infix operators drag precedence rules a neophyte cannot predict
(`flags band mask shl 2`: what binds first?). The better direction, and the
one this note adopts, is that bitwise operations become **function-like
compiler intrinsics**, bare-name builtins in the style the language already
uses for `length`, `spawn`, and `append`: `bitand(a, b)`, `bitor(a, b)`,
`bitxor(a, b)`, `bitnot(x)`, `shiftleft(x, n)`, `shiftright(x, n)`. The names
run their words together with no underscore, because a reserved name (keyword,
builtin, intrinsic) carries no underscore in this language; the underscore is
left entirely to user identifiers (a compound concept is `bitand`, not
`bit_and`). The spelling reads as what it does, over the C-ish `band` (which
reads as "band", not "bit and"); `bitand` / `bitor` have a real C++
alternative-token precedent, and the terser assembler `shl` / `shr` remain an
option for the shifts, finalized when bitwise ships. A function call has one
obvious grouping and no precedence to learn, which is what makes it safe
surface to expose without teaching an operator hierarchy. So exclusive-or for
bits is the intrinsic `bitxor`, the core operator `xor` is not reserved for
it, and the word `xor` is simply gone.

### Migration

The lexer answers `xor` with a teaching hint: "there is no `xor`; two bools
compare with `!=`, which is true when exactly one is true, so `a xor b` is
`a != b`. If precedence bites in a larger expression, parenthesize." A plain
`a != b` is unchanged. This is the same migration-hint courtesy the other
retired spellings get.

## Survey

Where the boolean-xor need lands in other languages, and which reach the
beginner:

- **Scratch, Logo, and block/teaching environments**: `and` / `or` / `not`
  only. No `xor`. This is the trio Excelsior keeps, and the direct precedent
  for the audience.
- **Python**: no word `xor`; `!=` for bools, `^` for bitwise. A beginner uses
  `!=`. The model here.
- **C, Java**: `^` is bitwise; bool exclusive-or is `!=`. No word operator.
  (C++ offers `xor` as an alias for `^`, essentially unused.)
- **Pascal, Verilog, VHDL**: `xor` is a first-class keyword, because these are
  expert or hardware-description languages where exclusive-or is idiomatic.
  The opposite end from this audience, and the reason `xor` reads as
  expert-facing.
- **C, APL, and the `<<` `>>` `&` `|` `^` family**: bitwise as infix
  operators, each with its own precedence rank, a perennial source of
  "parenthesize your bit ops" bugs even for professionals. The anti-model for
  the intrinsic direction below: a function call has no such hierarchy.

Excelsior's stance: the three boolean words a beginner needs (`and`, `or`,
`not`), with bool exclusive-or spelled `!=` like Python and C, the word `xor`
retired rather than tiered, and bitwise operations, if they land, exposed as
function-like intrinsics rather than precedence-bearing infix operators.

## Decisions (confirmed)

**D1. Retire `xor` as a core word operator.** It is bool-only, `a xor b` is
exactly `a != b`, and it costs a keyword and a precedence slot for no new
capability. Per tiers.md, redundant surface is removed, not tiered.

**D2. The boolean operators are `and`, `or`, `not`.** Bool exclusive-or is
`!=`. The lexer answers `xor` with a migration hint pointing at `!=` and
noting the precedence difference (parenthesize if it bites).

**D3. Bitwise operations, if introduced, are function-like compiler
intrinsics, not infix operators.** Bare-name builtins in the `length` / `spawn` /
`append` style, spelled with the words run together and no underscore
(`bitand(a, b)`, `bitor(a, b)`, `bitxor(a, b)`, `bitnot(x)`, `shiftleft(x,
n)`, `shiftright(x, n)`), because a reserved name carries no underscore in
this language (a compound concept is `bitand`, not `bit_and`; the underscore
is left to user identifiers); the terser `shl` / `shr` remain an option for
the shifts, finalized when bitwise ships. A function call has one obvious
grouping and no precedence to learn, so bitwise stays out of the core operator
space and off the neophyte's operator-hierarchy burden. Exclusive-or for bits
is `bitxor`; the core word `xor` is not reserved for it. (Bitwise is not
shipped; this records the direction, superseding R18's infix `band` / `bor`
framing.)
