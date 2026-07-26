# String patterns: readable command grammar, matched fallibly

Status: decided (2026-07), not yet implemented. A strings-and-text pass from the backlog (backlog.md, TODO's
regex / string-pattern item). Tier: World (tier 1); parsing player input into a
command is what a story or map author does constantly, so the basic pattern is
tutorial surface, its typed holes Mechanics (tiers.md). Builds on Icon's
success/failure (fallible.md), Inform 7's command grammar (a cited influence),
the `${}` interpolation hole (string-plan.md), string views (string-repr.md),
and the text grain of text-encoding.md.

The use is parsing player input: turning `"put brass key in oak chest"` into a
verb and its arguments. The backlog framed it as a "POSIX regcomp-shaped
facility", but raw regex is the wrong surface for this language, its syntax is
cryptic and unsearchable, the opposite of words-over-symbols, and a runtime
regex engine is a sandbox hazard. This pass takes the backlog's real driver, the
**compile-time versus runtime string distinction**, and builds a readable
pattern that is checked and compiled ahead of time and matched fallibly.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

## Why not raw regex

Two reasons the primary surface is not `^(\w+)\s+(\d+)$`:

- **It reads like line noise.** A non-programmer cannot say a regex aloud, look
  it up, or debug it, and the design has spent the whole series moving structure
  onto words for exactly this audience. A regex is the densest possible pile of
  the symbols words-do-structure keeps out.
- **A runtime regex engine is a sandbox risk.** If a pattern can be built at
  runtime and compiled by an engine in the VM, a script can hand the server a
  catastrophically-backtracking pattern (ReDoS) or an arbitrary-cost match. An
  in-world script must not be able to do that.

The second point is also the backlog's design driver, and it points at the fix:
make patterns **compile-time literals**, checked and compiled when the script
is, so there is no runtime engine to abuse and every pattern's cost is known
ahead of time.

## The design

### A pattern is a template with typed holes

A pattern literal looks like a string with **capture holes**, the same `${}`
holes interpolation uses, read in the other direction:

    "get ${item}"
    "put ${item} in ${container}"
    "go ${dir is Direction}"
    "buy ${count is int} ${item}"

Between the holes are literal words that must match; each `${name}` is a
**capture** that binds a piece of the input. This is the `${}` hole's duality: in
a value position `"${x}"` **builds** (reads `x` and inserts it), in a pattern
position `"${x}"` **captures** (binds `x` from the input). One hole, read two
directions, the destructuring-mirrors-construction convention (Rust, JavaScript)
applied to strings. A pattern position is exactly the right-hand side of
`matches` and a `when` arm over a string subject, nowhere else, so the direction
is never ambiguous.

A hole may name a **type**, which both constrains the match and converts the
capture:

- `${item}` captures the text bounded by the adjacent literals, a **str view**
  into the input (string-repr.md, no copy).
- `${count is int}` matches a number and binds an `int`; a non-number fails the
  match.
- `${dir is Direction}` matches a member word of the enum and binds the enum
  value (Inform 7's "[a direction]"), so a bad direction simply does not match.

Typing the holes is what makes a pattern a parser rather than a substring
grabber: the arm body receives a real `int` or `Direction`, already validated,
not a string it must re-check. The exact tokenization grain (a hole as a single
word versus a run up to the next literal versus a greedy tail) follows
text-encoding.md's code-point rules and is a sub-decision of implementation; the
common template shape above is what this note fixes.

### Patterns are compile-time literals, checked and compiled

**A pattern must be a literal**, and the matcher accepts only a literal. Its
syntax is validated when the script compiles (balanced holes, known types, a
valid `is Type`), and it is **compiled to a static matcher baked into the
binary**, a small sequence of literal-compares and bounded hole-scans, not a
regex program run by an engine. A runtime string used as a pattern is a compile
error: `a pattern must be written literally; a computed pattern would need a
matching engine the sandbox does not run`. This is the compile-time versus
runtime string distinction the backlog named: a **pattern** is a compile-time
kind, checked and lowered ahead of time, distinct from the **runtime `str`** it
is matched against. The payoff is safety by construction, no ReDoS, no arbitrary
compilation, every pattern's match cost bounded and known.

### Matching is fallible, and binds the captures

A match **succeeds and binds the captures, or fails**, which is Icon's and
SNOBOL's success/failure model, already the language's (fallible.md). So a
pattern match is a fallible expression consumed by the binding forms, and it
extends `if var` from one binding to the pattern's several:

    if input matches "get ${item}" then
        take(item)                    // item is a str view, bound here
    endif

`input matches PATTERN` is the fallible test; consumed by `if` / `while` it
brings the pattern's holes into the branch scope (the `if var` family, one name
becoming the pattern's captures), and on no match the `else` runs or the failure
propagates. The captures are views into the input for text holes and converted
values for typed holes, so a successful parse allocates nothing.

### Command dispatch is `match` over a string

A verb parser is many patterns tried in order, which is exactly `match` with
pattern arms (match-when.md), and this is the headline surface:

    match input
        when "look"                     then describe(here)
        when "look at ${item}"          then describe(item)
        when "go ${dir is Direction}"   then move(dir)
        when "put ${item} in ${into}"   then stow(item, into)
        when "buy ${n is int} ${item}"  then buy(n, item)
        otherwise                            tell(player, "I don't understand.")
    endmatch

Each `when` arm is a pattern; the first that matches binds its holes and runs its
body. This gives string subjects to `match` for the first time (via patterns, so
the arms bind rather than test equality), and alternation falls out of writing
several arms (no in-pattern `|` needed): `"look"` and `"look at ${item}"` are two
arms, not one pattern with an optional tail. It is Inform 7's "Understand ...
as ..." grammar in the language's own `match`.

## What is deferred

- **In-pattern optionals, alternation, and repetition** (`[at]`, `north|n`,
  `${x}+`). Multiple `when` arms cover alternation and optional tails, and an
  enum covers a fixed word set (`Direction`), so the common command parsing needs
  none of it; richer in-pattern structure waits for a demonstrated need.
- **A raw-regex escape.** If a genuine regex need appears (extracting structured
  text, not parsing commands), it would be a **compile-time-literal** regex
  compiled to the same static matcher, a distinct Mechanics or library facility,
  never a runtime engine. Deferred, and gated the same safety way.
- **Runtime-built patterns.** Excluded by construction, not deferred; the whole
  safety story is that a pattern is a literal.

## Survey

- **Icon / SNOBOL**: string pattern matching as a success/failure expression,
  the model where a match binds or fails. Excelsior's fallible match is this,
  and SNOBOL is the ancestor of pattern-matching-as-control-flow.
- **Inform 7 "Understand ... as ..."**: readable command grammar with typed
  tokens (`[a direction]`, `[something]`) for interactive fiction, the exact
  audience and use; the `match input / when "..."` form is this grammar.
- **scanf / Python `parse`**: typed template extraction (`%d`, `{name:int}`),
  the readable inverse of `printf` interpolation, which is the `${}` duality
  here.
- **Rust `if let` and the compile-checked `regex` crate**: pattern binding into
  a branch, and regex validated at build time; Excelsior takes both, the
  branch-binding and the compile-time check, and drops the runtime engine.
- **POSIX `regcomp` / `regexec`**: the mechanism the backlog named; kept, but
  run at compile time on a literal, never at runtime on a computed pattern.
- **Perl / PCRE**: the cryptic-and-powerful pole, and a runtime engine with
  ReDoS; the surface and the runtime-compilation this note declines.

Excelsior's stance: Inform 7's readable typed-hole command grammar, matched with
Icon's success/failure and bound like `if let`, over patterns that are
compile-time literals checked and compiled ahead of time (POSIX-shaped but never
a runtime engine), with captures as string views.

## Decisions (confirmed)

The six decisions are confirmed. The implementation (a pattern-literal reader
and compile-time syntax check, the static-matcher lowering, the fallible
capture-binding through the `if var` machinery, and the string-subject `when`
pattern arms over match-when.md) follows; it depends on match-when.md for the
arm form and string-repr.md for the capture views.

**D1. The pattern surface is a template with typed named holes, not raw regex.**
`"put ${item} in ${container}"`, `${x is int}`, `${x is Direction}`; literals
between holes must match, a hole captures. Raw regex is rejected as the surface
(cryptic, unsearchable, and a runtime-engine hazard).

**D2. A `${}` hole builds in a value position and captures in a pattern
position**, the destructuring-mirrors-construction duality; pattern positions
are the right of `matches` and a `when` arm over a string subject, nowhere else.

**D3. A pattern is a compile-time literal, syntax-checked at compile time and
compiled to a static matcher baked into the binary.** A computed/runtime pattern
is a compile error. This is the compile-time-versus-runtime string distinction:
a pattern kind distinct from a runtime `str`, and it is the safety story (no
runtime engine, no ReDoS, bounded match cost).

**D4. Matching is fallible (Icon/SNOBOL): it binds the captures or fails.**
`input matches PATTERN` is consumed by `if` / `while`, extending `if var` from
one binding to the pattern's captures; text captures are str views
(string-repr.md), typed holes bind converted values, so a parse allocates
nothing.

**D5. Command dispatch is `match` over a string with `when PATTERN then` arms**,
the first match binding and running (Inform 7's Understand grammar). This gives
`match` string subjects via patterns, and alternation is several arms, so
in-pattern `|` / optionals are unneeded.

**D6. In-pattern optionals/alternation/repetition, a compile-time-literal raw
regex escape, and (excluded, not deferred) runtime patterns are out of v1.** The
typed-hole template plus multiple `when` arms and enums cover command parsing;
richer structure and a regex escape wait on a demonstrated need and keep the
compile-time-literal safety model.
