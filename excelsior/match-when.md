# Making match boundaries explicit: the `when` arm keyword

Status: decided (2026-07); implemented (D1-D5): `when` opens every arm in both
the statement and expression forms, the subject is bounded by the first `when`
(so its trailing newline is optional), the `at_arm_start` LL(2) lookahead is
deleted, and the Trap 1 parenthesize hint retired. Deferred with then-in-if.md:
full newline-transparency inside the subject region (D2's wrapping half).
A surface pass revisiting the match arm boundary, prompted by
then-in-if.md's principle (close a construct on an explicit word, not a
newline). Tier: World (tier 1); `match` is core control flow the tutorial
teaches (tiers.md). Revises case-select.md's arm-shape decision and retires
parse-traps.md's Trap 1.

then-in-if.md closed the if/while/for headers on explicit words (`then`, `do`)
so a newline is never the sole boundary. `match` is the construct still leaning
hardest on the newline: its subject ends at a newline, and its arm bodies have
**no terminator at all**, so the parser finds each arm boundary by peeking at
the start of the next line (the `at_arm_start` LL(2) rule). That lookahead is
the newline-as-crutch this review targets, and it is the direct cause of the
one arm-steal misparse the language has to teach around. This pass gives every
arm an explicit opener, `when`, so the boundary is a word.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

## Where match leans on the newline

Two boundaries in match are set by the newline rather than a keyword:

    match direction
        north then go(0, 1)          // arm 1
        south then                    // arm 2, multi-statement body
            play(step_sound)
            go(0, -1)
        east then go(1, 0)           // arm 3: how does the parser know arm 2 ended?
        otherwise stop()
    endmatch

- **The subject** (`match direction`) ends at the newline, like `if` and
  `while` did before then-in-if.
- **An arm body** has no closing token. After arm 2's `go(0, -1)`, the parser
  decides whether the next line (`east then go(1, 0)`) continues arm 2 or starts
  a new arm by inspecting how the line begins: the `at_arm_start` rule reads an
  arm start when a line begins with a constant-label form (a literal, `-`,
  `true`/`false`, or an IDENT whose next token is `,`, `to`, or `then`). This is
  a two-token lookahead over the line break, the newline doing structural work.

That lookahead is fragile at exactly one point, and the language already has to
teach around it. **Trap 1** (parse-traps.md): a body line that is an
if-expression statement headed by a bool variable, `locked then "..."`, begins
`IDENT then` and is misread as a new arm labelled `locked`. The parser sees an
arm where the author wrote a conditional. Today's remedy is a check-time
teaching error that says "parenthesize the condition"; the ambiguity itself
stays.

## Why revisit now

case-select.md weighed an arm keyword against bare labels and chose bare labels,
to save a keyword and keep arms terse; parse-traps.md's survey recorded that
"that decision stands; this pass does not reopen it," and reduced Trap 1 to a
teaching error instead. Both were right under the weighting of the time, which
prized terseness. But parse-traps.md D1 left the door open: revisit if a broader
surface change lands.

**then-in-if.md is that change.** It re-weighted the whole surface toward
explicit boundaries over newline-inferred ones, closing every conditional header
on a word. Under that weighting the match arm boundary is the last and worst
newline crutch: not merely an implicit boundary but a *lookahead-driven* one
with a live misparse. Making it explicit is now the consistent move, not the
terse one.

## The design: every arm opens with `when`

**Each arm begins with the keyword `when`.** The arm is `when LABELS then BODY`,
and the body runs until the next `when`, the `otherwise` default, or `endmatch`:

    match direction
        when north then go(0, 1)
        when south then
            play(step_sound)
            go(0, -1)
        when east then go(1, 0)
        when west then go(-1, 0)
        otherwise stop()
    endmatch

`when` does all the delimiting the newline used to guess at:

- **The subject is bounded by the first `when`** (or `otherwise` / `endmatch`
  for an empty match), a keyword the subject expression cannot absorb, so the
  subject/arms boundary is explicit with no `of` word needed. Like the
  if-condition region (then-in-if.md), the subject between `match` and the first
  `when` is newline-transparent.
- **Each arm boundary is the `when` keyword.** The parser starts an arm exactly
  when it reads `when`, with no lookahead: a body line that does not begin
  `when` is a body statement, full stop. The `at_arm_start` LL(2) rule is
  deleted.
- **The body still holds ordinary newline-separated statements.** The newline
  keeps its normal job (separating statements within a body) and loses the
  special job (guessing arm boundaries).

The `otherwise` default is unchanged: it is the one arm with no label, so it
takes no `when` and no `then`, exactly as `else` is the label-less if arm
(`otherwise BODY`, closed by `endmatch`). Labels, value lists, and `lo to hi`
ranges are unchanged (`when 1, 2, 5 then`, `when 3 to 8 then`). The
expression-position match is identical in shape, an arm body being one
expression: `when L then EXPR`.

## What this retires

- **The `at_arm_start` lookahead rule** (parse.c) is gone; an arm starts at
  `when`, strictly LL(1).
- **Trap 1, the arm-steal, is eliminated structurally, not taught around.** A
  body line can never be read as an arm, because an arm must begin with `when`
  and a statement never does. parse-traps.md's Trap 1 teaching error (the
  widened `check_match_arm_labels` message) retires with the ambiguity it
  described. A label that is a lone identifier is now unambiguously in label
  position (after `when`, before `then`), so a variable used as a label is a
  clean "labels must be constant" error, not an arm-steal.
- **parse-traps.md is fully discharged.** Its Trap 2 (the greedy-comma `select`)
  already retired when choosers.md deleted `select`; this pass retires Trap 1.
  Both documented misparses are now removed by structure, so the teaching-error
  band-aids give way to grammar that cannot misparse.

This also makes precise the line then-in-if.md left open, that "match stays
self-delimiting": it self-delimits **through `when`**, an explicit word, rather
than through the newline lookahead that phrase glossed.

## The word: `when`

`when` is the arm word from the construct's own lineage. case-select.md credits
"Ruby's `when ... then`" as the arm-shape source, and Ada spells a case arm
`when L =>`; the surface reads as plain English for the audience, "when north,
then go" (Inform-7-style readability, a core goal). It was set aside only to
save the keyword.

The keyword was "earmarked for a library macro kind", the illustrative reactive
trigger among core.md's `effect/dialog/after/when` lib-macro examples. Core
syntax has the stronger claim: match is universal and this retires a parse trap,
while the trigger macro is an unbuilt, illustrative framework word that can take
another spelling (`whenever`, `after`, or `rule`; `on` is spoken for by `on
fail`). So `when` is reclaimed for match arms.

The alternative is **`case`**. It carries no earmark, and case-select.md's own
reason for renaming the head from `case` to `match` ("the arms are the cases, so
the old head named the wrong operation") actually endorses `case` as the *arm*
word, its correct home. `match E ... case L then ...` reads in the C / Rust /
Swift idiom. It is the safer pick if disturbing the `when` earmark is unwelcome;
`when` is the pick if audience-readable English wins, which is this note's
recommendation.

## The cost, reweighted

The objection that sank the arm keyword before was noise: a word on every arm.
That cost is real and unchanged. What changed is the scale it is weighed on.
then-in-if.md already decided the language pays for explicit boundaries
(`then`, `do`, the per-kind `end<kind>` terminators), because an explicit word
is easier to read and impossible to misparse. `when` is the same coin: it is the
noise of `endif` and `then`, spent to delete a lookahead rule and a taught
misparse. Under the surface's current, explicitness-first weighting, that trade
is worth making; under the terseness-first weighting of case-select.md, it was
not. The reversal is a reweighting, not a contradiction.

## Survey

- **Ruby**: `case E when L then ... when L then ... else ... end`, the exact
  `when ... then` arm the lineage credits; Excelsior differs only in the head
  (`match`) and the per-kind `endmatch`.
- **Ada**: `case E is when L => ... when others => ... end case`, the explicit
  `when` marker with a non-newline arm boundary. The precedent parse-traps.md
  named.
- **Rust / Swift**: `match`/`case` with `=>`/`:` arm arrows, an explicit
  per-arm marker; Excelsior keeps the marker but spells it a word, not an arrow
  (symbols do math, words do structure).
- **C `switch`**: `case L:` per arm with fallthrough and `break`; the explicit
  `case` marker without the fallthrough footgun.
- **ML / Haskell**: leading `|` per arm, the terse symbolic marker Excelsior
  declines for a searchable word.
- **Erlang**: `case E of L -> ...; ... end`, arms separated by `;`; the
  expression-always model case-select.md drew the arm shape from.

Excelsior's stance: Ruby's `case`/`when`/`then` arm, spelled `match` / `when` /
`then`, with the explicit `when` marker restored so the subject and every arm
close on a keyword and the newline never sets a structural boundary.

## Decisions (confirmed)

The six decisions are confirmed, with `when` the arm word (D5, `case` not taken).
The implementation (deleting `at_arm_start`, the LL(1) `when` arm parse, the
`when`-bounded subject, and retiring the `check_match_arm_labels` Trap 1 message)
follows; `when` is added to the keyword set and reclaimed from the lib-macro
earmark.

**D1. Every match arm opens with `when`: `when LABELS then BODY`.** The body
runs to the next `when`, `otherwise`, or `endmatch`. This holds in both
statement and expression position (an expression arm body is one expression).

**D2. The subject is bounded by the first `when`** (or `otherwise` / `endmatch`),
a keyword boundary, newline-transparent like the if-condition region; no `of`
word is added. The `otherwise` default is unchanged (label-less, no `when`, no
`then`, like `else`), as are labels, value lists, and `to` ranges.

**D3. The `at_arm_start` LL(2) lookahead is deleted; arms are LL(1) on `when`.**
The newline returns to its ordinary role of separating statements within a body
and never sets an arm boundary.

**D4. Trap 1 is eliminated structurally, and parse-traps.md is fully
discharged.** A body line cannot be misread as an arm; the widened
`check_match_arm_labels` teaching error retires. Trap 2 having already retired
with `select` (choosers.md), both documented misparses are now gone by
grammar.

**D5. The arm word is `when`**, reclaimed from the illustrative reactive-trigger
lib-macro earmark (core syntax outranks an unbuilt framework word; the trigger
takes `whenever` / `after` / `rule`). `case` is the recorded alternative if the
earmark is to stand; the recommendation is `when` for audience-readable English.

**D6. This is a reweighting of case-select.md and parse-traps.md, not a
contradiction.** The arm keyword was rejected under terseness-first weighting;
then-in-if.md moved the surface to explicitness-first, which is the "broader
change" parse-traps.md D1 named as the trigger to revisit.
