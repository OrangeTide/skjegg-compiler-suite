# The fallback vocabulary: `else`, `otherwise`, `on fail`

Status: implemented (2026-07). Sorts out the words for "the other branch"
and "when there is no value / the action failed", after review found
`else` doing two unrelated jobs (a boolean branch and a value fallback)
and found `on fail` too failure-specific to name the value fallback (a
`select` default is "none of these", not a failure). It revises
else-operator.md's naming of the fallback operator and sits alongside
fallible-consumers.md, which introduced the statement `on fail` handler.
All five decisions are confirmed and in the tree; see the implementation
note at the end.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

## The problem

`else` carries two unrelated jobs: the boolean branch (the statement
`if`, the if-expression `cond then A else B`) and the value fallback
(`xs[i] else 0`, `select i from a, b, c else d`). Inside an `if` the two
collide one line apart, told apart only by position:

    if checkthing() else false      -- else here: value fallback
        good()
    else                            -- else here: the branch
        bad()
    endif

fallible-consumers.md already moved statement failure-handling off `else`
onto `on fail`. A first sketch tried to also rename the value fallback to
`on fail` and to give `on fail` two forms (a value form and a statement
form) chosen by position. Two objections sank that:

- `on fail` is too failure-specific for the value cases. `select i from
  "red", "green", "blue" on fail "unknown"` reads worse than `else` does:
  an out-of-range index is "none of these", not a dramatic failure.
- One word with two position-chosen meanings is the same
  position-dependence that made `else` confusing in the first place.

## The design: three words, one role each

Give each job its own word, and give each word exactly one role, so no
word changes meaning with position.

| word | role | form | example |
|---|---|---|---|
| `else` | the other boolean branch | statement / if-expression | `if b ... else ... endif`, `cond then A else B` |
| `otherwise` | the fallback value when a fallible yields none | expression operator | `xs[i] otherwise 0`, `select i from a, b, c otherwise d` |
| `on fail` | recover from a failing action | statement handler | `reserve(seat) on fail alert()`, `step() on fail break` |

The rule is now three crisp sentences a beginner can hold at once:
`else` picks between two boolean branches; `otherwise` supplies a backup
value when a fallible expression produced none; `on fail` runs a recovery
action when an action fails.

`else` never touches failure again. `otherwise` is purely a value
operator (it always yields a value). `on fail` is purely a statement
handler (it never yields a value). Nothing is position-overloaded.

### Why two failure words, not one

`otherwise` and `on fail` are not redundant; they are the value job and
the action job, and keeping them apart is what removes the
position-dependence. Zig is the precedent: `x orelse 0` coalesces a null
optional to a value, `x catch |e| { ... }` handles an error union with an
action, and they are deliberately two keywords, not one used two ways.
Here:

- **`otherwise`** answers "what value instead?" It operates on a
  value-producing fallible (a `maybe T`, an index, a divide, a
  `select`/`match`, a `returns maybe T` call) and yields a value. It
  cannot contain `break` or `return`, because it is an expression.
- **`on fail`** answers "what do I do about the failure?" It operates on
  an action (a `can fail` call, or any fallible statement) and runs a
  handler statement, which may be a call or a control transfer
  (`break`, `continue`, `return`, `fail`). It yields nothing.

You reach for `otherwise` when filling in a value and `on fail` when
reacting to a failed action, which is just the expression-versus-statement
line the language already draws everywhere.

## What each evaluates to

This answers the question the sketch left muddy.

- **`A otherwise B` yields a value.** `A` and `B` share a common type and
  that is the result type. `var w is int = xs[i] otherwise 0` is an int.
  It is an expression and only an expression; there is no statement form,
  so there is nothing to be confused about.
- **`stmt on fail handler` yields nothing.** It is a statement. The
  handler runs only on failure, for effect or control flow, and needs no
  return type: in `action() on fail recover()`, the whole thing is a
  statement, and `recover()` is not required to return a bool or anything
  else (a returned value, if any, is discarded like any
  expression-statement).

## Chaining

- **`otherwise` chains right-associatively as a value cascade.** `a
  otherwise b otherwise c` is `a otherwise (b otherwise c)`: try `a`, else
  `b`, else `c`, first success wins, evaluated lazily and left to right,
  lowest precedence (a whole expression binds to its left). If `b` is
  itself fallible, `a otherwise b` stays fallible, so `b`'s failure flows
  to `c`; an unconsumed tail propagates or traps like any failure. Depth
  is unbounded. `xs[i] otherwise ys[j] otherwise 0`.
- **`on fail` chains by statement nesting.** `a() on fail b() on fail
  c()` is `a() on fail (b() on fail c())`: run `a`; on failure run [run
  `b`; on failure run `c`]. A cascade of recovery attempts.

## The word for the value fallback

`otherwise` is the recommendation: it reads naturally in every position,
including the `select` case that started this (`select i from ...
otherwise d`), and it is plainly not `else`. Two alternatives:

- **`orelse`** (one token, Erlang-style): shorter, but it visually
  contains "else" and risks the very confusion the rename removes.
- **`default`**: reads as "defaults to" for values (`xs[i] default 0`)
  but does not carry the sense across a `select`/`match` default as well,
  and is a common field name.

The one real cost of `otherwise` is length: it is the most common
fallback operator and `xs[i] otherwise 0` is wordier than `xs[i] else 0`.
For a beginner-facing language the readability is judged worth the
characters; `orelse` stays on the table if brevity wins.

## Scope: `select` and `match` defaults

With `otherwise` reading naturally (unlike `on fail`), the `select`
out-of-range default and the `match` no-match arm should adopt it, so
`else` is left meaning only a boolean branch:

    select i from "red", "green", "blue" otherwise "unknown"

    match kind
        1 then ...
        otherwise ...
    endmatch

This keeps the one-rule clarity ("`else` is bool, `otherwise` is a
fallback value"). The alternative is to leave `select`/`match` `else` as
construct-internal keywords (they do not collide with `if`), accepting
that `else` then still appears as a default in those two constructs. The
recommendation is to move them to `otherwise`, since the whole point is
that `else` stops meaning "no value".

## Decisions (confirmed)

**D1. `else` is reserved for boolean branches only.** The statement `if`
else and the if-expression `else` keep the word; `else` never names a
value fallback again.

**D2. The value fallback operator is `otherwise`, replacing the `A else B`
operator.** An expression operator that yields a value; right-associative,
lazy, lowest precedence, as the `else` operator is today. (`orelse` was
weighed as a terser spelling and set aside: it visually contains "else".)

**D3. `on fail` stays the statement failure handler.** Unchanged from
fallible-consumers.md. It is the action-recovery word, distinct from the
value word `otherwise`, so each word keeps a single role and neither is
position-overloaded. (The alternative, collapsing both onto one word,
reintroduces the two-forms position-dependence and is rejected.)

**D4. `select` and `match` defaults become `otherwise`.** So `else` means
a boolean branch and nothing else.

**D5. Answers to the two review questions (recorded, not a choice).**
`otherwise` yields a value and is expression-only; `on fail` yields
nothing and is statement-only; both chain (a right-associative value
cascade, and nested statement handlers, respectively).

## Implementation notes

Lexer and parser only; the AST and the checker/lowering machinery are
unchanged, since only the surface keyword moved.

- **Lexer**: a new `T_OTHERWISE` keyword (`otherwise`).
- **Parser**: the fallback operator in `parse_expr` reads `T_OTHERWISE`
  instead of `T_ELSE` (the `cond then A else B` if-expression keeps
  `T_ELSE`); `parse_select` and both `match` parsers take `T_OTHERWISE`
  for the default. The node kinds are untouched: the fallback operator
  is still the `N_ELSE` node internally (its surface is now `otherwise`),
  and `select`/`match` defaults are still the same `->c` child, so the
  checker and lowering needed no structural change, only their
  user-facing `else` messages retargeted to `otherwise`. The statement
  `on fail` handler (fallible-consumers.md) is unchanged.
- **Grammar**: grammar.ebnf updated so `expr` ends `... "otherwise"
  expr`, and the `match` / `select` productions take `otherwise`; `else`
  survives only in `if_stmt` and the if-expression.

Every test that used `else` as a value fallback, a `select` default, or a
`match` default arm was migrated to `otherwise` (the `if` and
if-expression `else` left alone). A new `exs_otherwise.exs` pins the
operator, the right-associative chaining cascade, and `else`/`otherwise`
coexisting in one verb. check-exc 47/47.

The old spelling `xs[i] else 0` gets a targeted teaching error rather
than a generic "expected end of line": `parse_expr` catches an `else`
that immediately follows a bare expression (a legitimate `else` follows
`then A` in the if-expression, or a newline and a body in an `if`
statement, never a bare expression), and points at `otherwise`. This
fires uniformly across every context the operator can appear in
(statement, `var` init, assignment, an interpolation hole, a call
argument), since they all route through `parse_expr`.
