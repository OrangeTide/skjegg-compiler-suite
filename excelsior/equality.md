# Equality: `=`, not `==`

Status: decided (2026-07), implemented. A surface-and-readability pass
from the backlog (backlog.md). Equality is `=` and inequality `<>` in the lexer
(a stale `==`/`!=` gets a migration hint), the parser reads `=` as equality in
an expression while a statement's leading postfix (an lvalue or call) stops
before the assignment `=`, and the exs tests use the new spelling. Tier: World (tier 1); a comparison is in the first program an
author writes. Builds on the binding `=` (var/const), the type word `is`
(`var x is integer`, and `x is T` narrowing, union-types.md), and the
value-equality records want (records.md D5).

`==` is the wrong spelling for this audience. The `=` (binding) versus `==`
(equality) split is the classic beginner trap, C's own `if (x = 5)` bug in
spirit, and a doubled symbol reads as noise in a language that spends its whole
design moving structure onto words.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

## Not `is`: that is already the type word

The tempting readable answer, `pos is target`, is wrong, because **`is` is
already the type word** and the most pervasive one: `var x is integer`, `field
hp is int`, `param p is Point`, and the `x is T` type test in an expression
(union-types.md). Spelling equality `is` too would overload the keyword an author
reads on nearly every declaration line, and `x is integer` would have to be told
apart from `x is 5` by whether the right side is a type or a value. That is one
word doing too much. `is` stays the type word only.

## The real fix: `=` is math's equality symbol, misassigned

The language's own principle is **symbols do math, words do structure**.
Equality is math, and math's symbol for it is `=`. Yet Excelsior currently gives
that symbol to **binding** (structure) and hands **equality** (math) the doubled
C token `==`. That is backwards. The fix is to put `=` back where math has always
had it:

- **Equality is `=`.** `if pos = target`, `if hp = 0`, `if name = "Frodo"`. This
  is math, Pascal, SQL, and the chalkboard, the spelling every beginner already
  reads as "equals".
- **Inequality is `<>`.** `if pos <> target`, Pascal's and SQL's "not equal",
  which reads as "differs" and joins the ordering symbols rather than carrying
  C's cryptic `!`.
- **`==` and `!=` retire**, answered by the lexer with a migration hint.

## Binding keeps `=`, resolved in the parser

Binding and assignment stay `=` (`var x = 5`, `const k = 3`, `x = expr`), and
there is no ambiguity, because of **where the parser consumes the token.** A
`var`/`const` or an assignment statement parses its target and then **eats the
`=` itself, before the expression parser ever runs**; the parser then reads the
right-hand side as an expression. So a `=` reached *inside* an expression is
always equality, there is no separate assignment operator to confuse it with, and
C's `if (x = 5)` bug cannot occur here at all. The symbol reads uniformly as
"equals": asserted when binding (`x` is now 5), tested when comparing (`x` equals
5?).

The one consequence is that a **complex lvalue that embeds a comparison needs
parentheses**, since the statement's `=` is claimed by the assignment. That case
(assigning through a target computed with an `=` comparison) is rare, and the
parentheses make the intent explicit where it arises.

If an explicit assignment operator is ever wanted instead, the recorded
alternative is a **left arrow `<-`** (`x <- 5`), preferred over Pascal's `:=`. It
is the identical "becomes" concept, it is the assignment symbol of mathematical
and algorithmic notation (`x ← 5`) and the APL / Smalltalk lineage rendered in
ASCII, and it pairs cleanly with `=`-for-equality the way math pairs `←` and `=`.
It is a *left* arrow, distinct from the retired *right* arrow `->` (the de-arrow
decision). This note still keeps binding as `=`, because eating the assignment
`=` at statement level already removes every collision, so no assignment operator
is needed; `<-` is the fallback if that parser rule is ever felt to be too
implicit.

## `=` means "the same thing": value or identity by kind

`=` reads as "is this the same?", and what "the same" means follows the kind,
consistent with the value/actor split (records.md):

- for a **value** (int, decimal, str, bool, enum, record), `=` is **value
  equality**: two records are equal when their fields are (records.md D5), two
  strings when their contents are.
- for an **actor** (`obj`), `=` is **identity**: the same actor, since an actor
  has identity and a variable holds a reference to it.

So `pos = target` compares two coordinates by value and `player = other` asks
whether they are the same actor. A record with an `obj` field therefore compares
that field by handle (identity), the records.md D5 subtlety, which falls out of
this one rule rather than being a special case.

## What stays

- **Ordering is unchanged**: `<`, `<=`, `>`, `>=` are math and stay symbolic;
  `=` and `<>` now join them as the comparison symbols. Equality moving to `=` is
  what makes the comparison family uniform, not the odd one out.
- **`is` stays the type word only** (declaration ascription and the `x is T`
  test).
- **`=` does not chain.** `a = b = c` is binding-of-a-comparison in a statement
  and a comparison-of-a-comparison in an expression, not a three-way equality;
  an equality chain is rare and unclear, matching `!=`'s non-chaining
  (CLAUDE.md).

## Survey

- **Pascal / Delphi**: `=` equality, `<>` inequality, `:=` assignment; the
  teaching-language model and the audience's lineage (the sibling Compact Pascal
  front end), adopted here with binding kept as `=` rather than `:=`.
- **SQL / BASIC / spreadsheets**: `=` for equality, the spelling a
  non-programmer has already met; BASIC even uses one `=` for both roles,
  disambiguated by position as here.
- **Smalltalk**: `=` value equality versus `==` identity; the value-versus-
  identity distinction adopted, folded into the one `=` by kind rather than two
  operators.
- **Mathematics**: `=` is equality; giving it to binding and equality to `==`
  was the inversion this pass undoes.
- **C / Zig**: `==` / `!=`, the spelling retired; **Inform 7 / AppleScript**:
  `is`, the readable-but-here-unavailable word (it is the type word).

Excelsior's stance: equality is `=` and inequality `<>`, reclaiming math's own
symbol from binding per symbols-do-math; binding stays `=` (safe because
assignment is a statement), ordering stays symbolic, and `is` stays the type
word.

## Decisions (confirmed)

The five decisions are confirmed and implemented. The lexer answers a stale `==`
with a hint to `=` and `!=` with a hint to `<>`, and lexes `<>` as the not-equal
token; the parser's comparison level accepts `=` (T_ASSIGN in an expression) as
the equality op and maps it to the T_EQ AST node, so the checker and lowering are
unchanged, and a statement parses its leading postfix (an lvalue or call) so the
following `=` is assignment, not an equality. Record value-equality (records.md
D5) uses `=`.

**D1. Equality is `=`, inequality is `<>`; `==` and `!=` retire** with a lexer
migration hint. `=` is math's own equality symbol (symbols do math), reclaimed
from the current backwards spelling that gave `=` to binding and `==` to
equality.

**D2. Binding and assignment keep `=`, resolved in the parser.** A `var`/`const`
or assignment statement eats its `=` before the expression parser runs, so a `=`
reached inside an expression is always equality and C's `if (x = 5)` bug cannot
occur. The only consequence is that a complex lvalue embedding a comparison needs
parentheses, which is rare. If an explicit assignment operator is ever wanted,
the recorded alternative is a **left arrow `<-`** (math's and APL/Smalltalk's
`←` in ASCII, the identical "becomes" concept, a *left* arrow distinct from the
retired `->`), preferred over Pascal's `:=`.

**D3. `=` means "the same thing": value equality for a value, identity for an
`obj`.** Two records are equal when their fields are (records.md D5); two actors
are `=` when they are the same actor, so a record's `obj` field compares by
handle. One rule, not a special case.

**D4. Ordering and `is` are unchanged.** `<`/`<=`/`>`/`>=` stay symbolic, joined
now by `=`/`<>`; `is` stays the type word only (ascription and the `x is T`
test).

**D5. `=` is binary and does not chain** (like `!=`); an equality chain is rare
and unclear.
