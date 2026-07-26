# Approachability review: findings against the audience objective

Status: design review (2026-07). A pass over the whole design-note
series against the stated objective, a language approachable for
beginner programmers and non-programmer content creators. This note
records what the review found: deviations from the objective, gaps,
redundancy, and overtuned corners. Nothing here is decided; each
finding carries a recommendation to be accepted, rejected, or reworked
in its own design pass. The doc drift the review also found (core.md's
stale Q-format and colon sends, string-plan.md's old slice spellings,
verbs.md's string-pick references) was fixed directly and is not
repeated here. R1, R2, and R8 were taken up by runtime-errors.md and
its v1 is implemented (per-site fault descriptors with teaching
messages, fallible divide/modulo, the OVERFLOW fault family, the -t
no-match trace event). R10 and R16 were taken up by output.md and its
v1 is implemented (tell to the console player through real sends,
trace/`///` as the -t author channel, log retired). R3 and R17 were
taken up by numbers.md and implemented past the finding: binary fixed
itself was replaced by base-10 `decimal` (0.1 * 3 == 0.3 exactly),
literals became untyped decimal constants defaulting to it, and the
`0f`/`fixed(num, den)` apparatus dissolved rather than being tiered.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

Overall verdict first: the design is coherent, and its principles
(words over symbols, teaching errors, one `else` concept, the
quotation boundary) serve the audience well. The findings below are
where it falls short of its own objective, ranked within each section.


## Deviations from the objective

### R1. "Errors teach" stops at compile time

The runtime failure story is the opposite of teaching. `__exc_trap`
prints one generic line ("trap: no branch chosen and no else") and
exits 70, with no source location, no offending value, and no
indication of which index or match failed. The Inform 7 study
identified cryptic off-happy-path errors as that language's core
failure, but the lesson was only applied to the compiler. For
non-programmers a runtime trap with no location is arguably worse than
a compile error, because it happens in front of players. No design doc
specifies trap diagnostics.

Recommendation: make runtime diagnostics a design item with the same
weight as the teaching compile errors. Candidates: a span table mapping
trap sites to source locations, a per-site trap message ("chest.exs
line 12: index 4 of a 3-element list"), and the static introspection
index as the delivery channel. This is the largest single gap relative
to the audience.

### R2. The silent no-op is designed in

A `match` statement with no `else` and no matching arm does nothing, by
design (case-select.md, "like `if` without `else`"). For a
debugging-naive builder, "my script ran and nothing happened" is the
classic dead end, and it is Inform 7's rulebook-didn't-fire trap in
miniature.

Recommendation: require `else` on the statement form too, or emit a
compile warning when the label set is visibly non-exhaustive, or make
the no-op case visible through the trace channel.

### R3. Numeric inference steers beginners into the weakest type

`var speed = 1.5` infers `float`, and float is second-class everywhere:
no float fields, no float-element lists, no `maybe float`, no float
through sends, no float match subjects, no float comparison chains. The
type the design wants stat math in (`fixed`) has the expert-facing
literal: `0f0.1` is a compile error because 0.1 is not dyadic, which is
a brutal first contact with the exact-or-error rule. The friendly path
exists (a plain float literal in fixed context rounds), but nothing
routes a beginner onto it.

Recommendation: pick one of three fixes before the audience arrives.
Infer `fixed` for decimal literals, or land float's missing pieces so
the inferred type is not full of walls, or write the "numbers for
builders" rule ("whole numbers are int, decimals are fixed, float is
for mechanics") and enforce it through inference.

### R4. Documented parse traps contradict "reads fine, works fine"

Two are acknowledged in grammar.ebnf: a line starting `IDENT then`
inside a match body is misread as an arm (the condition must be
parenthesized), and `select`'s greedy branch commas mean a select
inside an argument list must be parenthesized. Each is small, but "the
code reads right and misparses, and the fix is invisible punctuation"
is precisely the failure mode the surface was designed to avoid.

Recommendation: at minimum, dedicated teaching errors for both shapes.
The match case may also argue for tightening the arm-start rule; the
select case is an argument for R13.

### R5. The story persona gets the least type checking

Dialog trees, the flagship content-creator artifact, are quoted data
typed `any` "until the prop world types it for real" (grammar.ebnf,
data literals). So the persona the static-typing pitch aims at is the
one whose content flows through the untyped corner. A misspelled head
word or a missing choice target in a dialog literal is exactly the
mistake static checking exists to catch.

Recommendation: prioritize a schema mechanism for quoted data (a typed
data-literal design) proportional to that persona's importance. It need
not wait for the full prop design; a per-head shape declaration checked
by the reader would catch most of it.

### R6. Two absence words

`nothing` is "no result", `nil` is "no object". The distinction is
principled, and the checker forbids `maybe obj`, but a beginner will
meet both words and must learn which contexts want which. The
mitigation is entirely error-message quality.

Recommendation: keep the design; add a dedicated teaching error for
each direction of the mix-up (`nil` where a maybe is wanted, `nothing`
where an obj is wanted), in the same spirit as the removed-syntax
hints.

### R7. Text is bytes, and the audience writes text

Strings are byte-length descriptors; `s[i]`, `length`, and `for c in s`
operate on bytes. Content creators will write accented characters and
non-Latin text on day one, and byte indexing splits code points. No
doc states a policy, not even "v1 is bytes, indexing non-ASCII is
unsupported". Related smaller point: the language folds identifier
case, but string comparison is byte-exact; worth one sentence since
case-insensitivity is advertised as a principle.

Recommendation: decide and document the v1 encoding stance, and check
what the client actually feeds the VM. If UTF-8 text passes through
whole (concat, holes, output) and only indexing is byte-level, the
honest v1 answer may be "strings are UTF-8, indexing is for ASCII
protocol text, use `for c in s` and slicing for display text", but
that only works if those iterate code points, which today they do not.


## Gaps

### R8. Arithmetic failure policy is unstated

Division lowers to plain `IR_DIVS` with no zero check, so `10 / 0` is
whatever the hardware does under the VM (SIGFPE under qemu), which is
neither the trap nor a fallible expression. The fallibility note
settles indexing and calls but never mentions `/`, `%`, or overflow.

Recommendation: decide among trap, fallible, or defined wrap. The Icon
model gives a principled slot: a fallible `/` composes as `x / y else
0` with the existing machinery and costs nothing when consumed. Whatever
is chosen, int overflow deserves a stated policy in the same pass,
since "drift-free stat math" is a headline claim.

### R9. Trap versus clamp is split across indexing and slicing

`xs[i]` out of range fails (trap when unconsumed), but slices
`xs[a to b]` clamp. A builder who learns "out of range fails, catch it
with `else`" then finds `s[5 to 99]` silently succeeding. Either
policy is defensible; having both is a rule with an asterisk.
(string-plan.md now documents the asymmetry; this item is about whether
it should exist.)

Recommendation: consider making an empty-result slice fallible too
(`xs[a to b] else d`), or state the deliberate rationale for clamping
(slices are range intersection, indexes are element access) in
fallible.md where the policy lives.

### R10. Output is the biggest functional hole

Acknowledged in string-plan.md and host-abi.md, restated here as a
priority: "show text to the player" is the first thing every persona
needs, and it is still a provisional `log()` with the real capability
shape (bare power versus send to a well-known object) undecided. The
design cannot be validated against its audience until the audience's
first line of code has a real home.

### R11. Enum sequencing dead-ends

Enums got the showcase word-list syntax, but enum values do not lower,
and enum-member match labels wait on them. The construct most likely to
appear in a beginner's first class (item kinds, directions) parses and
then stops. Low-hanging sequencing fix: lower enums as small ints and
let match labels name members.

### R12. No defined beginner subset

Three personas share one substrate, but no document defines which slice
of the surface each persona is expected to touch. `fixed(num, den)`,
`quote`, `can fail`, `xor`, `band`/`shl`, and the meta layer are all
mechanics-tier or below, yet the grammar and keyword list present one
flat language.

Recommendation: a short "tiers of the surface" note stating what the
tutorial teaches and what it deliberately omits. It doubles as a
forcing function: every future feature must name its tier.


## Redundancy

### R13. Four choosers overlap, and `select` is the weakest

The if-expression, the `match` expression, `select`, and fallible
indexing with `else` all pick a value. `select idx from a, b, c else d`
is nearly `["a" "b" "c"][idx] else d` now that list literals exist; its
distinguishing features (lazy branch evaluation, the jump-table
promise) matter to the compiler, not the builder. It also carries one
of the two documented parse traps (R4) and a unique keyword (`from`).

Recommendation: one of three. Fold it into `match` on ranges; keep it
but exclude it from the tutorial tier (R12); or keep it and write the
"which chooser when" paragraph. Right now a beginner has four ways to
choose and no guidance.

### R14. `can fail` and `returns bool` are near-duplicates

A `can fail` call "types as bool (success)", so `if validate(key)` and
`if is_valid(key)` read identically at the call site while being
different mechanisms with different rules (a discarded `can fail`
result traps; a discarded bool is fine). The failure-is-not-a-value
principle is compromised exactly here, and a builder has two spellings
for "did it work".

Recommendation: re-examine before it fossilizes. Either drop `can
fail` and let valueless fallible funcs return `bool`, or drop the
bool-typing convenience so `can fail` stays purely in the fallibility
system and `if validate(key)` gets its own consumer form.

### R15. Member visibility is stated twice

`verb` is legal only under `public` and `func` only under `private`, so
the section adds no information for members; it exists for fields. A
builder writes the fact twice and gets an error when the copies
disagree. The teaching error is good; the ceremony is not.

Recommendation: let `verb`/`func` appear in either section (or outside
sections) with visibility implied by the keyword, keeping sections for
fields. The one-sentence rule ("public verbs are what others can ask
you to do, private funcs are how you do it") survives unchanged.

### R16. Three debug-output forms

`trace expr`, `///` trace comments, and the provisional `log()` are
three spellings of "show me something". `log` is scheduled to retire,
and `trace`/`///` are complementary (expression versus text), but the
trio should be reconciled in one doc so the tutorial teaches exactly
one.


## Overtuned

### R17. The fixed-point exactness apparatus sits in the core surface

`0f` exact-or-error literals, `fixed(num, den)` with power-of-two
denominators, hex arguments: DSP-grade tooling for what the audience
experiences as "a number with a decimal point". The machinery is
justified for drift-free stat math, but it belongs behind the
mechanics tier (R12), and the beginner-facing story ("write 1.5, it
rounds") should be the documented default rather than a footnote. See
also R3, which is the same tension from the inference side.

### R18. `xor` as a core word operator

Bool `xor` is `!=` on bools; the audience will never write it, and it
occupies a keyword and a precedence slot. Bitwise was exiled to
`band`/`bor` spellings for exactly this reason; word `xor` arguably
belongs with them.

### R19. `quote` leaks the meta layer into the surface

The meta layer is otherwise cleanly hidden, but `quote` sits in
`primary` and in the keyword list. If it is "rare, mostly lib-author
code", it is the one visible leak of the concatenative layer into the
grammar every builder sees.

Recommendation: gate it the way macro definition is gated, or at least
keep it out of the tutorial tier and the builder-facing keyword
documentation.

The sources and continuation pump design (else-operator.md) was
examined under this heading and passes: it is explicitly deferred and
second-class by design. Hold that line when `for in` over sources
lands.


## Priority

If only three findings get design passes before the next feature work:
R1 (runtime error UX, with R2 and R8 riding along, since all three are
"what does the builder see when it goes wrong at runtime"), R10
(output), and R3 (the beginner numeric path). Those are where a
non-programmer's first week actually happens; the rest is polish by
comparison.
