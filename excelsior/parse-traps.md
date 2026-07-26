# Parse traps: making the two documented misparses teach

Status: decided and implemented (2026-07), the pass on
approachability.md's R4 (the two documented parse traps contradict
"reads fine, works fine"). All four decisions at the end were confirmed
and are in the tree: a teaching error for the match arm-boundary trap
and a parse-time parenthesize guard for the greedy-comma `select`. One
refinement landed during implementation (D2: the parenthesize hint fires
only for a variable-bound label, since an unresolved name is the typo'd-
constant case); the implementation notes at the end record it.

Superseded (2026-07): both traps are now gone by grammar, so neither
teaching error survives. Trap 1 retired with the `when` arm keyword
(match-when.md D4), which makes an arm boundary one keyword of lookahead,
so no body line can be stolen as a label. Trap 2 retires with `select`
itself (choosers.md, decided, its implementation pending), which is where
the greedy-comma guard goes with it. The note is kept for the reasoning
and the record.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

## The problem

Two ambiguities are acknowledged in grammar.ebnf. Each is small, but the
shared failure mode is the one the readable surface was built to avoid:
the code reads correctly, misparses, and the error blames something the
author never wrote. The fix is invisible punctuation.

**Trap 1, the match arm boundary.** Inside a `match` body, a line
starting `IDENT then` is read as a new arm (`at_arm_start`, the LL(2)
rule: an IDENT whose next token is `,`, `to`, or `then` begins an arm).
So a bare if-expression statement led by a bool variable is stolen as an
arm whose label is that variable:

    match n
        1 then
            locked then act(true) else act(false)   -- meant as a statement
        2 then
            n = 7
    endmatch

The author wrote a conditional; the parser saw an arm labelled `locked`.
The reported error is `match label \`locked\` must be a constant`, which
names a construct the author never intended. (When the stolen body also
fails to parse, the error moves further away still, blaming a `=` or a
stray token.)

**Trap 2, the select branch commas.** `select`'s branches are a greedy
comma list, so a `select` sitting unparenthesized in a call's argument
list eats the following arguments:

    return add(select i from 10, 20, 99)   -- add wants two args

The `99` was meant as `add`'s second argument; the select swallowed it as
a third branch. The reported error is `call to \`add\` expects 2
arguments, got 1`: a count mismatch that points at the call, not at the
missing parentheses. The fix is `add((select i from 10, 20), 99)`.

Neither trap is silent. Both produce a *misleading* error that sends the
author looking in the wrong place. That is the specific harm: a beginner
cannot connect `must be a constant` or `expects 2 arguments` back to "put
parentheses here."

## What exists today

- `at_arm_start` (parse.c) is a two-token lookahead: a leading literal,
  `-`, or `true`/`false` starts an arm immediately; a leading IDENT
  starts one only when followed by `,`, `to`, or `then`. This is the
  shipped, documented rule (case-select.md, "Plan"); it is deliberate
  and simple, and it is why bare constant labels (`red then ...`) can
  head multi-statement arms without an arm keyword.
- The constant-label check (typecheck.c, `check_match_arm_labels`):
  `if (lab->kind == N_NAME && (!lab->sym || lab->sym->kind != SYM_CONST))
  terr("match label \`%s\` must be a constant")`. This is the message
  Trap 1 surfaces. It already has the resolved symbol in hand, so it
  already knows whether the "label" is a variable.
- `select` parses its branches as a comma loop above the `else` operator
  (parse.c, `parse_select`); case-select.md records "parenthesize the
  select to put a fallback on the whole thing" and "a select passed as
  one of several call arguments needs parentheses." The trap is the
  documented consequence, not a bug.

The through-line: the parser's behavior is correct and intended in both
cases. The gap is entirely in the message.

## Survey

**Disambiguating an arm boundary.** Languages that let an arm hold many
statements use one of two devices. A syntactic marker on every arm (ML's
leading `|`, Ada's `when`, Rust and Swift's `case`/`=>`) makes the
boundary unambiguous but taxes every arm with noise. Or bare
constant labels plus a lookahead rule (Excelsior's choice, Ada-flavored
values), which reads cleanly but leaves exactly one ambiguity: a label
that is a lone identifier is indistinguishable from an identifier that
begins a statement, until you know whether the identifier is a constant.
case-select.md weighed the arm keyword and rejected it (it costs a
keyword already earmarked, and noise on every arm). That decision stands;
this pass does not reopen it. What it leaves is a boundary that only the
*checker* (which has the symbol table) can adjudicate, which points at a
check-time message, not a parse-time rule change.

**The error message as the remedy.** Elm and Rust treat a diagnostic as
a product surface: where an ambiguity is unavoidable, the fix is a
precise, actionable message that names the correction, not a
restructuring of the grammar. Excelsior already commits to this voice
(runtime-errors.md's teaching faults, the lexer's migration hints for
removed syntax). Trap 1 is a textbook case: the checker holds every fact
needed to say "you wrote a variable where a label goes; if you meant an
if-expression, parenthesize the condition."

**Greedy variadic separators.** A comma-separated operand list is
unusual as a *sub*-expression; most expression grammars bound an operand
by precedence, so nesting is free. C's comma operator is the near
precedent: it must be parenthesized inside a call's argument list
(`f((a, b), c)`) precisely because the argument separator and the comma
operator collide. `select`'s branch list is the same shape, a mini
argument list, so it collides with any enclosing argument list. The
precedent-backed fix is the same: require parentheses when the greedy
form is nested in another comma context, and say so when it is not.

## Design

**Trap 1: turn the constant-label error into a teaching error.** Keep
`at_arm_start` unchanged. In `check_match_arm_labels`, when the offending
label is a lone IDENT that resolved to a variable (a local, param, or
field), extend the message to name the if-expression fix:

    match label `locked` must be a constant; if you meant an
    if-expression here, parenthesize the condition:
    `(locked) then A else B`

The parenthesized form dodges the heuristic cleanly: a line starting `(`
is not an arm start, so the parser reads the statement. The message is
widened only when the label resolves to a variable, which cannot be a
constant label and is exactly the if-expression case; a genuine typo'd
or undeclared constant (`gold` misspelled) stays unresolved and keeps
the plain "must be a constant", since suggesting parentheses there would
mislead. This is the whole fix for the realistic case, where the if-
expression's branches are values and the stolen body parses.

The residual case (branches that are assignments, so the stolen body
fails to parse before the checker runs) is left as is: assignments are
not valid if-expression branches anyway, so that error, while still
placed oddly, is reporting a real second mistake. Chasing it would mean a
parse-time reinterpretation the two-token grammar cannot support.

**Trap 2: require parentheses around a nested `select`.** At parse time,
when about to read a comma-list element (a call argument, or a branch of
another `select`) and the next token is `select`, reject it with a
teaching error rather than letting the branch commas run:

    a bare `select` in an argument list eats the following commas as its
    own branches; wrap it in parentheses:
    `add((select i from 10, 20), 99)`

This converts the silent argument-eating into a precise message at the
offending token. It is a three-line guard in the argument and branch
loops. A single-argument call pays a small, predictable tax
(`f((select ...))`), which buys a rule with no exceptions: a `select` in
a comma list is always parenthesized, the way C's comma operator is.

**Relation to R13.** R13 questions whether `select` earns its keep at
all (it overlaps the if-expression, `match`, and list indexing, and it
carries exactly this trap). If R13 later folds `select` into `match` or
retires it, both Trap 2 and this guard retire with it. The guard is
low-regret insurance in the meantime, not a commitment to keep `select`.

**The principle.** No construct should silently take tokens across a
boundary that reads as closed, and where the grammar is deliberately
ambiguous (the arm boundary) the error must name the invisible fix. The
two traps are the only two the grammar documents; this pass closes both
against that principle without reopening the surface decisions
(constant labels, no arm keyword) that created them.

## Staging

1. **Trap 1 teaching error.** Widen `check_match_arm_labels` when the
   label is a variable-bound or unresolved lone IDENT. Self-contained,
   no parser change.
2. **Trap 2 paren guard.** Reject an unparenthesized `select` at the
   head of a call argument and a `select` branch, with the teaching
   message. Self-contained, parser only.

Both are small and independent; either can ship without the other.

## Decisions (confirmed and implemented)

**D1. Trap 1: teaching error, keep the rule.** Widen the existing
constant-label message to name the parenthesize fix when the label is a
bound variable. The alternative, tightening `at_arm_start`, cannot work
without either forbidding bare-IDENT constant labels (breaking the common
`red then` enum-member arm) or adding the arm keyword case-select.md
already rejected.

**D2. Trigger the wider message on a variable-bound lone IDENT.** Fire the
parenthesize hint only when the label resolves to a local, param, or
field. Refined during implementation: the note first included unresolved
names, but an unresolved name *is* the typo'd-constant case (a misspelled
constant does not resolve), so it keeps the plain message; only a
variable, which can never be a constant label, gets the hint.

**D3. Trap 2: require parentheses around a nested `select` (pending
R13).** An unparenthesized `select` in a comma list (a call argument or
a `select` branch) is now a parse-time teaching error. Implemented as a
cheap guard now; R13 may retire it along with `select`.

**D4. Leave the bare if-expression statement legal.** `cond then A else B`
stays a legal expression statement everywhere; the teaching error (D1) is
the proportionate fix for Trap 1. Revisit only if a broader "expression
statements must have an effect" rule is ever adopted.

## Implementation notes

Both fixes are in the front end, small and independent:

- **Trap 1** (`typecheck.c`, `check_match_arm_labels`): when a lone-IDENT
  label is not a `SYM_CONST`, and it resolved to a `SYM_LOCAL`,
  `SYM_PARAM`, or `SYM_FIELD`, the message names the `(cond) then A else
  B` fix; otherwise (unresolved, or another symbol kind) the plain "must
  be a constant" stands.
- **Trap 2** (`parse.c`, `guard_bare_select`): called at the head of each
  call argument (`parse_arg_list`) and each `select` branch
  (`parse_select`); an unparenthesized `select` there is rejected with
  the parenthesize message. A single-argument call now writes
  `f((select ...))`, the accepted tax for a rule with no exceptions.

The canonical `tests/exs_select.exs` was updated to the parenthesized
form, so it doubles as the rule's worked example. The two error paths
have no positive artifact to assert (the harness runs no
expected-compile-failure cases); they were verified by hand:

    match label `locked` must be a constant; if you meant an
    if-expression here, parenthesize the condition: `(locked) then A else B`

    a `select` in a comma list must be parenthesized; its branch commas
    are greedy and would otherwise eat the following items:
    `f((select i from a, b), x)`
