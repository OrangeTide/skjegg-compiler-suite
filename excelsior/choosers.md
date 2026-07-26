# Four choosers, minus one: retiring `select`

Status: decided (2026-07), not yet implemented. The pass on
approachability.md's R13 (four value-choosers overlap, and `select` is the
weakest). Now that list literals exist, `select` is subsumed by two
constructs that already ship, so it retires. What is left is three choosers
with three distinct selection criteria and one sentence of guidance apiece,
which is the "which chooser when" map R13 asked for. The five decisions are
confirmed; the removal (lexer hint, dropping `parse_select`/`N_SELECT` and
the Trap-2 guard, migrating `exs_select`) is the follow-up.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

## The problem

Excelsior has four ways to pick a value, and a beginner meets all four with
no guidance about which to reach for:

- **the if-expression** `cond then A else B`: pick by a **yes/no test**.
- **the `match` expression** `match E ... endmatch`: pick by **what a value
  is**, its identity or which label/range/set it falls in.
- **`select`** `select i from a, b, c otherwise d`: pick the i-th of a
  fixed list of **branch expressions**, by position, lazily.
- **fallible indexing** `xs[i] otherwise d`: pick the i-th element of a
  **data list**, by position.

The first two carry distinct criteria (a condition; a value's identity).
The last two both pick by position, and they overlap. R13's specific
observation is that `select i from a, b, c otherwise d` is nearly
`[a b c][i] otherwise d` now that list literals exist, and that `select`'s
distinguishing features (lazy branch evaluation, the jump-table promise)
matter to the compiler, not to the builder. On top of the overlap,
`select` costs a unique keyword (`from`) and one of the two documented
parse traps (R4's greedy-comma trap, parse-traps.md Trap 2).

## What `select` is today

`select` parses, type-checks, and lowers. `select i from a, b, c
otherwise d` evaluates `i`, then evaluates only the i-th branch (1-based),
falling to `otherwise d` (or trapping) when `i` is out of range. Its
branches are a greedy comma list, which is why an unparenthesized `select`
in an argument list eats the following commas (Trap 2). case-select.md kept
it, on two grounds: it is compact and positional, and it "can promise a
jump-table lowering".

Both grounds have since weakened:

- **Compactness** was `select`'s edge before list literals. `select i from
  "gold", "silver", "bronze" otherwise "none"` was the short way to pick
  the i-th of some values. With list literals, `["gold" "silver" "bronze"]
  [i] otherwise "none"` is just as short and reads more plainly: you see a
  list and an index, no keyword to learn.
- **The jump-table promise** is not unique to `select`. `match` labels are
  constants precisely so a `match` can lower to a jump table (enums.md, and
  the CLAUDE.md note that constant labels are "what a jump-table lowering
  wants"). A positional dispatch expressed as `match i` with int labels
  gets the same lowering. And list indexing is already O(1) by
  construction.

## `select` is subsumed

`select` has exactly two uses, and each maps onto a construct that already
ships:

**The eager case: pick the i-th of some values.** When the branches are
literals or cheap reads, `select` and list-indexing are interchangeable,
and the list form is the friendlier spelling:

    select i from "gold", "silver", "bronze" otherwise "none"
    ["gold" "silver" "bronze"][i] otherwise "none"      // same, clearer

**The lazy case: pick the i-th of some actions, run only that one.**
`select`'s one real semantic difference from list-indexing is laziness:
`[f() g() h()][i]` (via `${}` holes) builds the list first, so it calls all
three, while `select i from f(), g(), h()` calls only the chosen one. When
the branches are side-effecting, that difference is correctness, not just
cost. But `match` on an int subject covers it exactly, and lazily, arm by
arm:

    match i
        1 then f()
        2 then g()
        3 then h()
        otherwise wait()
    endmatch

`select` is, semantically, sugar for a `match` whose labels are the
consecutive integers 1..N. It adds no selection criterion the other three
lack: it picks by position, and position is already served (eagerly by
`[...][i]`, lazily and dispatched by `match i`). Retiring it loses no
capability. It removes a keyword, a fourth overlapping chooser, and a parse
trap.

## The design: retire `select`, keep three distinct choosers

Remove `select` and the `from` keyword. The lexer answers `select` (and a
stray `from`) with a migration hint, the same courtesy the other removed
constructs get (the colon send, `?:`, `num:den`, binary `fixed`, `->`/`..`).
The hint points at the two replacements:

    select i from a, b, c otherwise d
      -> for a value pick:   [a b c][i] otherwise d
      -> for lazy actions:   match i / 1 then a / 2 then b / ... endmatch

Trap 2 and its parse-time parenthesize guard retire with `select`, as
parse-traps.md already anticipated ("if R13 later folds `select` into
`match` or retires it, both Trap 2 and this guard retire with it"). That
leaves R4 with one trap, not two.

What remains is three choosers, each keyed to a genuinely different
question, which is the guidance R13 wanted:

- **by a yes/no** -> the if-expression: `cond then A else B`.
- **by what a value is** (a kind, a number, a range, an enum member) ->
  `match`.
- **by a position in a list** -> `list[i] otherwise d`.

A condition, an identity, a position. No two of these answer the same
question, so "which chooser when" is a three-line table a tutorial can
teach in one breath, instead of four constructs with a silent overlap.

## Survey

The dedicated positional-branch chooser is unusual. Most languages give you
two tools and expect the positional pick to fall out of them:

- **C, Ada, Verilog, Pascal**: a `switch`/`case` keyed on a value. This is
  `match`. A positional pick is `case i of 1: ...; 2: ...`, i.e. a match on
  consecutive labels, exactly the reduction proposed here.
- **Lisp `case`, ML/Rust/Swift `match`/`switch`**: the same, with richer
  labels (lists, ranges, patterns). None ships a separate index-the-branches
  form.
- **APL / array languages, Python, Perl**: positional pick is array or dict
  indexing, `values[i]`. This is the list-index form.
- **Verilog's `casez`, computed `goto`, a raw jump table**: the closest kin
  to `select`, and the tell is that these are compiler-level or
  hardware-level constructs. `select` exposed a jump table as surface
  syntax; the audience does not ask for a jump table, it asks to pick the
  i-th thing.

So the near-universal split is indexing (eager, positional) plus a
value-keyed `switch` (lazy, dispatched). Excelsior already has both. A
third construct sitting between them is the redundant one, which is why
`select` reads as the weakest of the four.

## Supersedes

This note supersedes case-select.md's section 2 ("`select` stays, as the
positional sibling"). That decision was correct when it was made, before
list literals existed and before the jump-table promise had a home on
`match`. The verbal family it aligned (`if ... else`, `case ... of`,
`select ... from`, `A else B`, one preposition each) loses its `from` row;
the remaining three still share the fallback vocabulary (`otherwise` for
the value fallback, per fallback-words.md, which already replaced the
shared `else` that section leaned on).

## Decisions (confirmed)

**D1. Retire `select` and the `from` keyword.** It is subsumed: the eager
positional pick is list indexing `[a b c][i] otherwise d`, and the lazy or
side-effecting positional dispatch is `match i` with int labels, which also
inherits the jump-table lowering. `select` adds no selection criterion the
other three choosers lack.

**D2. The lexer gives a migration hint for `select` (and a stray `from`),**
pointing at both replacements, as the other removed constructs do. Trap 2
and its parenthesize guard retire with `select`, leaving R4 with one
documented trap.

**D3. The three remaining choosers key to three distinct questions,** and
that is the "which chooser when" guidance R13 asked for: a yes/no ->
`cond then A else B`; what a value is -> `match`; a position in a list ->
`list[i] otherwise d`.

**D4. This supersedes case-select.md's "select stays".** The compactness
and jump-table grounds it rested on are now served by list literals and by
`match`'s constant labels, respectively.

**D5. Migration is follow-up.** The one test that exercises `select`
(`exs_select`) moves to the list-index spelling, and the removal (lexer
hint, dropping `parse_select`, `N_SELECT`, and the Trap-2 guard) lands with
the implementation, not this note.
