# One switch: unifying the `case` statement and the `select` expression

Status: design note (2026-07). The `case` statement and the `select`
expression shipped separately and look nothing alike. The survey and
proposal below unify them without ambiguous or lookahead-heavy grammar.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

UPDATE (2026-07, after implementation): the construct was renamed from
`case E of` to `match E` with `endmatch`. The head now names the
operation (the arms are the cases, so the old head named the wrong
thing), and the subject is a bare expression ended by the
newline, like `if` and `while`, so the `of` keyword is gone from the
language. A second decision then removed the arrow language-wide
(symbols are unsearchable; words read aloud): an arm is `labels then
body`, the else arm is a bare `else`, return types use `returns`, and
ranges use `to` instead of `..`, so the arm-start rule reads "IDENT
followed by `,`, `to`, or `then`". A third decision then renamed the
default arm and the value fallback operator from `else` to `otherwise`
(fallback-words.md), so `else` means only a boolean branch; read the
`else` defaults and the `A else B` operator below as `otherwise`. The
analysis below predates all three decisions and uses the old spellings.


## What we have today

    case n of                       // statement: match values
        1, 2 -> self.greet()
        3..5 -> self.wave()
        else -> self.shrug()
    endcase

    select i from "a", "b", "c" else "?"    // expression: pick by position

Two problems. First, the surfaces are unrelated: `of` versus `from`, arms
versus a comma list, `endcase` versus nothing. A new user has to learn two
constructs for one idea, "choose a branch". Second, they differ in kind:
`case` matches values, `select` indexes positionally. That difference is
real and worth keeping, but the surfaces should still rhyme.


## Survey

- **C, Java `switch`**: statement only, fallthrough by default. The
  fallthrough footgun is the classic new-user trap. Nothing to copy.
- **C# `switch`**: has BOTH a switch statement (`case X:`) and a switch
  expression (`X => v`), with different arm syntax, different default
  syntax (`default:` versus `_`), and different semantics (the expression
  throws on no match). Widely confusing for learners. This is the
  anti-goal: one idea, two unrelated spellings.
- **Ada, Pascal `case`**: statement only, no fallthrough, constant labels
  with ranges and value lists. Our arm labels (`1, 2` and `3..5`) already
  follow this family.
- **Erlang `case E of ... end`**: an expression, always. Arms are
  `pattern -> body`, the last body value is the result. Our statement
  syntax borrowed this shape.
- **Rust `match`, Zig `switch`**: one form that IS an expression;
  using it as a statement just discards the value. Arms are expressions,
  and a block is an expression, so nothing is duplicated. This is the
  strongest model for "the statement and the expression feel like one
  thing": there is only one thing.
- **Python `match`**: statement only; the asymmetry with expression-
  oriented peers is a recurring complaint. Confirms that shipping only a
  statement form leaves a gap.

Lesson: don't invent a second spelling for the expression form (C#).
Either make the one form usable in both positions (Rust/Zig/Erlang),
or make the two forms share every keyword and arm shape.


## Proposal

### 1. `case` is one construct, legal in both positions

Keep the statement exactly as shipped and allow it in expression
position; the only difference is what an arm body is:

    // statement position: arm bodies are statements
    case n of
        1, 2 -> self.greet()
        else -> self.shrug()
    endcase

    // expression position: arm bodies are single expressions
    var name = case n of
        1 -> "one"
        2, 3 -> "few"
        else -> "many"
    endcase

Same keywords, same arm shape, same `else`, same `endcase`. The rule a
new user holds is one sentence: "a case arm body is a statement where a
statement goes, and a value where a value goes."

An expression `case` with no `else` traps when nothing matches, exactly
like `select` with no `else` and like `xs[i]` out of range. See point 3.

### 2. `select` stays, as the positional sibling

`select i from a, b, c else d` earns its keep: it is compact, it is
positional rather than matching, and it can promise a jump-table lowering.
Recasting it as a `case` over 1..N would lose the one-liner and blur the
"index picks" reading. Instead we align the family verbally:

    if C ... else ...
    case E of ... else -> ...
    select I from ... else D
    A else B

One preposition per construct (`of` matches, `from` indexes), and `else`
means the same thing in all four.

### 3. `else` is the fundamental operator underneath

All four constructs above are "a chooser that can fail to choose, plus an
`else` that catches the failure". That is the `A else B` operator applied
to a fallible chooser. Lowering already wants this shape: each of them
compiles to tests that either branch to a value/body or fall through to
one shared else-label; a missing `else` puts a trap at that label
(`__exc_trap`, exit 70). Making the else-label the common lowering spine
means `case`, `select`, indexing, and any future fallible form get the
fallback semantics for free, and a stand-alone `A else B` is just the
degenerate chooser.

Follow-on (future, not now): once `case` works in expression position, an
expression `if` may be unnecessary; `case cond of true -> a else -> b
endcase` covers it, and a shorter sugar can be revisited later.


## Parsing analysis

- **Expression `case` is LL(1).** `case` in primary position is keyed by
  the keyword; arms are `case_vals "->" expr NL`; `endcase` closes. The
  parser skips NL tokens inside the construct, the way an open paren
  already continues lines. No conflict with the statement form, because
  they are the same parse; the context (statement or expression position)
  only decides how arm bodies are parsed.
- **Multi-statement arms need an arm-start rule.** With one statement per
  arm (shipped v1) there is nothing to decide. With multi-statement
  bodies, after each NL the parser must tell "next arm" from "next
  statement of this arm", and both can start with an identifier or a
  literal. Two clean options:
  1. Restrict `case_vals` to constants (literals, enum names, ranges),
     Ada-style, and use the rule: a line that contains `->` before its NL
     starts an arm. `->` appears nowhere in an expression or statement, so
     a bounded scan to the end of the current line is deterministic and
     cheap. Constant labels are also what the jump-table lowering wants.
  2. Introduce an arm keyword (`when 1, 2 ->`). Strictly LL(1), but it
     costs a keyword that is already earmarked for a library macro kind,
     and it adds noise to every arm.
  Recommendation: option 1. Constant labels plus the scan-for-arrow rule.
- **`select` needs no change.** Its branches parse above the `else`
  operator, so the trailing `else` binds to the select; parenthesize the
  select to put a fallback on the whole thing.


## Plan (implemented 2026-07)

All four steps landed:

1. `case_vals` are constant labels: literals (optionally negated ints,
   `0f` fixed, `true`/`false`) or a module-const name. Enum-member
   labels arrive when enum values lower at all.
2. Multi-statement arms. The implemented arm-start rule is simpler and
   stronger than the scan-for-arrow option above: because labels are
   constants, a line starting with a literal, `-`, `true`/`false`, or
   an IDENT whose next token is `,`, `..`, or `->` begins an arm.
   Plain LL(2) via a two-token lexer lookahead; no line scanning.
3. Expression-position `case` with single-expression arms, sharing the
   statement's label parsing. A statement case with no `else` and no
   match is a no-op (like `if` without `else`); the expression form
   traps (exit 70), like `select`.
4. `select` and the `case` expression share one fallback tail
   (`lower_else_or_trap`: store the `else` value, or call
   `__exc_trap`). Fallible indexing keeps its inline value fallback,
   since its `else` always has a value and never traps; the shared
   piece is the trap-or-else tail, which is where the lowerings
   actually overlapped.
