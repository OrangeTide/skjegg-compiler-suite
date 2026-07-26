# Closing a condition: `then` on `if`, `do` on loops

Status: decided (2026-07); implemented (D1-D6): `then` closes an `if` and
`elseif` condition, `do` closes a `while` and `for` header, the region between
the head keyword and its closer is newline-transparent, a single statement may
sit inline after the closer, and a missing closer is a local teaching error.
One refinement landed during implementation (D5, recorded at the end): an
unparenthesized if-expression in a condition is a teaching error naming the
parentheses, not a silent greedy parse, because there is no warning channel to
carry the suggestion the decision described.
A standalone surface pass from the backlog (backlog.md,
TODO's `then`-in-if item). Tier: World (tier 1); the if-statement is the first
control flow the tutorial teaches (tiers.md). Relates to sequences.md (the
newline policy), the if-expression (choosers.md), and parse-traps.md (the
disambiguation traps).

The if-*expression* closes its condition with `then` (`cond then A else B`), but
the if-*statement* does not: today it is `if COND` then a newline, and the
newline alone separates the condition from the body:

    if self.locked
        opener.tell("The chest is locked.")
        return
    endif

The backlog asks what that missing terminator costs. This pass answers: it
costs an implicit boundary where the language elsewhere insists on explicit
ones, and it splits the one idea "test a condition" into two shapes. The fix is
to close a condition with `then` on `if` and `elseif`, matching the
if-expression and the Algol/Pascal/BASIC surface the audience reads.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

## What the missing `then` costs

The newline-terminated condition works, but it pays four prices:

- **An implicit boundary in a language built on explicit ones.** Every block
  closes with a named terminator (`endif`, `endwhile`), the quotation boundary
  is explicit, and errors are meant to teach from explicit structure (core.md).
  The condition/body split, alone, rests on a bare newline. A reader parses
  where the condition ends by noticing the line break, not a word.
- **Multi-line conditions need a continuation trick.** A compound condition
  that wraps must end each line on a binary operator (or an open bracket, or a
  trailing `\`) so the newline does not terminate it (sequences.md). The reader
  must spot the trailing `and` to know the condition continues.
- **No one-line guard.** The overwhelmingly common `if bad then return` guard
  clause cannot be written, because there is no marker for where the condition
  ends and the single guarded statement begins.
- **Two shapes for one idea.** A learner meets `cond then A else B` (the
  if-expression, with `then`) and `if cond` (the statement, without), and must
  learn that "test a condition" is spelled two ways. Pascal, BASIC, Lua, and
  Ada all write `if cond then`; the audience expects the word.

## The design: a condition is closed by `then`

**`if` and `elseif` close their condition with `then`.** The rule is one line:
a condition is followed by `then`, the same `then` the if-expression uses.

    if self.locked then
        opener.tell("The chest is locked.")
        return
    endif

    if key is BarrowKey then
        self.locked = false
    elseif key is Skeleton then
        rattle()
    else
        refuse()
    endif

The `else` arm has no condition, so it takes no `then`; `elseif` does. This is
Pascal/BASIC/Lua/Ada exactly, and it makes the statement rhyme with the
expression: `then` always follows a condition, whether the result is a branch
of control or a branch of value.

### Loops close their header with `do`

`while` has the same implicit-boundary problem, and so does `for`. They close
their header with **`do`**, not `then`:

    while hp > 0 do
        attack()
    endwhile

    for step in 1 to count do
        advance()
    endfor

`do` rather than `then` because the two words carry different intuitions the
audience already holds: `then` reads as "next, once" (a one-shot branch), while
`do` reads as "do this, repeatedly" (a loop). Using `then` on a loop would
fight that reading. This is the Pascal/Lua split (`if ... then`, `while ...
do`, `for ... do`), and keeping it costs one keyword, `do`, for a real
readability gain.

`for` gains an extra clarity from `do`: its header is `for IDENT in EXPR [to
EXPR]`, and today the end of the iterable, or of the optional `to`-range, is
marked only by the newline. `do` closes it explicitly, so both the list form
(`for item in inventory do`) and the range form (`for step in 1 to count do`)
end on a visible word rather than a line break, the same win `then` gives an
`if`. `match` keeps its current self-delimiting header: its subject
is followed by arm labels, not a body, and each arm already carries its own
`then`, so there is no bare condition/body boundary to mark.

### The condition is a newline-transparent region

Between the head keyword and its `then` / `do`, the condition is a
**newline-transparent region**, exactly like the inside of `( )` and `[ ]`
(sequences.md's continuation rule). A compound condition may break across lines
with no trailing-operator trick, because `then` / `do`, not a newline, closes
it:

    if hp > 0
       and mana >= spell.cost
       and not stunned then
        cast(spell)
    endif

So `then` does not merely restate the newline; it turns the condition into a
bracketed region the way parentheses do, which is what buys the free wrapping.
This is more explicit than today's boundary, not less: the region is closed by
a visible word instead of an invisible line break.

### The one-line guard

With an explicit close, the guard clause fits on one line:

    if bad then return endif
    while queue.more do process(queue.take) endwhile

After `then` / `do`, the body may either start on the next line (the block
form) or continue inline as a single statement closed by the block terminator.
It is a single statement because there is no `;` statement separator to put a
second on one line (sequences.md: newlines separate statements). A
multi-statement body uses the block form. The block terminator (`endif`,
`endwhile`) closes the inline statement in place of a newline.

## Disambiguating an if-expression in the condition

Because a condition is any expression, it may itself be an if-expression, and
the two `then`s must not collide. The if-expression's **mandatory `else`** is
what keeps them apart (choosers.md):

- `if a then ...` with no `else` on the line: `a then` cannot start an
  if-expression (that needs an `else`), so the condition is just `a` and the
  `then` closes the statement. The common case is unambiguous.
- `if (a then b else c) then ...`: an if-expression condition is parenthesized,
  and the outer `then` closes the statement. This is the clear form.
- `if a then b else c then ...`: written without parens, the parser greedily
  reads `a then b else c` as the if-expression condition, and the second `then`
  closes the statement. It parses, but the double `then` is a smell, so when a
  condition contains a top-level `then ... else` the checker suggests
  parenthesizing if that nesting was unintended. This joins the documented
  parse-trap family (the match arm-steal and the greedy comma, parse-traps.md),
  answered by the same parenthesize-guard teaching.

## Errors teach, and old code migrates

A missing `then` / `do` is caught locally: the condition region scans to the
next structural keyword (`endif`, `elseif`, `else`, `endwhile`, `endfor`, or
end of file) and, finding none, reports `an "if" condition ends with "then";
add "then" after the condition` (or `"do"` for a loop) at the head. The error
is local because the structural keyword bounds the scan, honoring the
resync-at-the-nearest-terminator rule (core.md).

Adopting `then` / `do` changes existing `then`-less code, which is mechanical:
`then` is added before the condition's newline, `do` before a loop body. The
lexer answers an old-form `if COND` whose next line is a statement (no `then`
seen) with the same teaching hint, so the migration is guided rather than
silent. This is a one-time surface change on a not-yet-frozen language, the same
class of change as the de-arrow and the retired spellings.

## Survey

- **Pascal / Object Pascal**: `if C then S`, `while C do S`, `for i := ... do
  S`, `case E of`. The direct model; Excelsior takes `then` for `if` and `do`
  for loops verbatim, and leaves `match` self-delimiting rather than adopting
  `of`.
- **BASIC**: `IF C THEN ...`, the `THEN` every dialect shares; the audience's
  first `then`.
- **Lua**: `if C then ... end`, `while C do ... end`, `for ... do ... end`, the
  same then/do split with a single `end` (Excelsior uses per-kind `endif` /
  `endwhile`).
- **Ada**: `if C then`, `while C loop`, `for ... loop`, `case E is`; the
  keyword-terminated header tradition.
- **Python**: `if C:` / `while C:` with the colon as the universal
  header-closer. A symbol, not a word, so Excelsior does not follow it (words
  over symbols); the colon is the same idea Excelsior spells `then` / `do`.
- **Ruby / Go**: Ruby's optional `then` and Go's brace-delimited condition show
  the two poles Excelsior rejects, an optional word (two shapes) and a
  brace-block (no condition terminator at all).

Excelsior's stance: Pascal/Lua's `then` for `if`/`elseif` and `do` for
`while`/`for`, a required word that closes a newline-transparent condition
region, enabling the one-line guard and matching the if-expression's `then`.

## Decisions (confirmed)

The six decisions are confirmed. The implementation (the `then` / `do`
terminators, the newline-transparent condition region, the inline one-liner, the
if-expression disambiguation, and the migration hint) follows; it touches the
lexer's newline handling and the parser's if/while/for headers.

**D1. `if` and `elseif` close their condition with a required `then`.** The
rule is "a condition is followed by `then`", the same `then` as the
if-expression, so the statement and expression share one shape. `else` (no
condition) takes no `then`.

**D2. Loops close their header with `do` (`while C do`, `for ... do`).** `then`
reads as one-shot and `do` as repeat, so the Pascal/Lua split matches the
audience's intuition; `match` stays self-delimiting (its arms carry `then`, its
subject is followed by labels, not a body).

**D3. The condition and loop header are a newline-transparent region between
the head keyword and `then` / `do`, like `( )`.** A compound condition wraps
across lines with no continuation trick; the visible word, not a newline,
closes it.

**D4. A one-line guard is `if C then STMT endif` (and `while C do STMT
endwhile`).** After `then` / `do` the body is either a next-line block or a
single inline statement closed by the block terminator; single, because there
is no `;` separator.

**D5. An if-expression condition is disambiguated by its mandatory `else`.**
`if a then ...` (no else) reads `a` as the condition; an if-expression
condition is parenthesized; the unparenthesized double-`then` gets a
parenthesize teaching hint, joining the parse-traps.md family.

**D6. A missing `then` / `do` is a local teaching error, and old `then`-less
code migrates with a lexer hint.** The region scan bounds the error at the next
structural keyword; the migration is mechanical and guided, a one-time surface
change like the de-arrow.

## Implementation notes

**D5 became an error, not a suggestion.** The decision said an unparenthesized
`if a then b else c then ...` parses greedily and the checker suggests
parenthesizing. The compiler has no warning channel, so the only two available
behaviors were silence or an error. It reports a teaching error at the second
`then`, naming the parenthesized form. The reasoning behind D5 is unchanged
(the mandatory `else` is what separates the two `then`s, and parentheses are
the clear form); only the strength of the response moved, and it moved toward
the "errors teach" rule rather than away from it. Adding the greedy parse back
is a one-line change if a warning channel ever lands.

**The condition case is not its own rule** (raised 2026-07). The two-`then`
collision looks like a clash between the if-statement and the if-expression,
but it is precedence. The if-expression is the lowest tier, so **everything to
the left of `then` is its condition**, and the same swallow happens with no
statement in sight:

    x = 1 + poisoned then 10 else 20      // the condition is `1 + poisoned`

So readers need one rule, not a condition-specific exception: **an
if-expression is parenthesized whenever it is not the whole expression.**
`1 + (a then b else c)`, `f((a then b else c))`, `if (a then b else c) then`.
The condition of an `if` statement is just where the precedence is most
visible, because the swallow leaves a second `then` on the line.

Both ways of breaking the rule now teach it. In a condition it is the parse
error above, reworded to state the rule rather than to describe conditions. In
every other position the swallowed left side used to surface as a type error
about the wrong thing (`1 + poisoned` reported as `arithmetic needs numeric
operands, got int and bool`, which names arithmetic the author wrote
correctly). The checker now carries a note through any error raised while an
if-expression's condition is checked, and the non-bool condition message states
the rule itself.

**How the two `then`s are told apart.** At a `then` inside a condition region
the parser scans forward for an `else` on the same line, then rewinds the
lexer. The scan stops at the line end, at a second top-level `then` or a `do`,
at a structural keyword, or at the close of an enclosing group, so a body
statement carrying its own if-expression is never mistaken for the condition.
Parentheses and a call's argument list clear the condition flag, which is what
makes `if (a then b else c) then` read normally.

**The region is a lexer counter.** `lex_cond_begin` / `lex_cond_end` bracket
the header and suppress the newline exactly as an open `(` does, so D3's free
wrapping costs one counter rather than a parser mode.
