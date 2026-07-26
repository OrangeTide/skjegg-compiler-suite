# Index fails, slice clamps: point access versus interval access

Status: decided (2026-07), not yet implemented. The pass on
approachability.md's R9 (trap versus clamp is split across indexing and
slicing). The decision is to keep the split, because it is the correct
semantics rather than an accident, and to close the learnability gap with
framing and a teaching error rather than by changing behavior. All four
decisions are confirmed; the follow-up is D2 (the fallible.md framing
sentence) and D3 (the slice-aware teaching error), both small. D1 keeps
the runtime unchanged.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

## The problem

Indexing and slicing disagree about the ends. `xs[i]` and `s[i]` are
fallible: an out-of-range index traps when unconsumed and is caught by
`otherwise` / `if var` (fallible.md). Slices `xs[a to b]` and `s[a to b]`
clamp: a low bound below 1 becomes 1, a high bound past the end becomes
the end, and an inverted or fully-past range yields an empty result.
Neither slice ever fails.

A builder who has learned "out of range fails, catch it with `otherwise`"
then meets `s[5 to 99]` on a five-character string succeeding silently
with the empty string, and `s[9 to 99] otherwise "?"` reporting "the left
side of `otherwise` cannot fail". The rule they learned has an asterisk,
and the asterisk shows up as a confusing error at the exact spot they
tried to apply the rule they were taught.

## What exists today

- **Index** is a fallible producer. The lowering bounds-checks and
  branches to the active fail label before the `__exc_str_at` /
  list-element load, so a bare `xs[i]` out of range traps (INDEX_RANGE,
  runtime-errors.md) and `xs[i] otherwise d` catches it. Point access:
  the one element at `i` either exists or it does not.
- **Slice** clamps, in `__exc_str_slice` / `__exc_list_slice`: `lo < 1`
  becomes 1, `hi > len` becomes `length`, `lo > hi` yields the empty
  string / list. It shares the parent buffer (strings) or copies the
  range (lists). It never branches to a fail label, so it is not a
  fallible producer, and a consumer on it (`otherwise`, `if var`) is
  rejected as having nothing to consume.

So the split is str-vs-list symmetric (both index, both slice behave the
same) and index-vs-slice asymmetric.

## Survey

The languages split on exactly this question, and the split is along a
clear line.

- **Python** has precisely this asymmetry, by design and for decades:
  `s[i]` raises `IndexError`, `s[a:b]` never raises and clamps
  (`"hello"[5:99]` is `''`). Python is the archetypal beginner language,
  and generations of beginners absorb the split without trouble, because
  each half is individually intuitive.
- **Rust** makes them uniform-strict: `v[i]` panics and `&v[a..b]` also
  panics out of range (no clamp); the fallible forms are `v.get(i)` and
  `v.get(a..b)`, both returning `Option`. Uniform, but a slice past the
  end is a panic, not a shorter slice.
- **Go** is uniform-strict too: `s[i]` and `s[a:b]` both panic out of
  range (a high bound past `length`/`cap` panics).
- **JavaScript / Ruby** are permissive both ways (`arr[i]` is
  `undefined` / `nil`, `slice` clamps), which trades the whole
  fallibility guarantee away and is not the model here.

The real choice is Python's split (point fails, interval clamps) versus
Rust/Go's uniform strictness (interval also fails). Excelsior already
took the fallible-index half; the question is the slice half.

## Design

**Keep the clamp. The asymmetry is the difference between two different
questions.** An index asks for a point: the single element at `i`, which
either exists or does not, so a miss is a genuine absence and fails. A
slice asks for an interval: the part of `a to b` that the value actually
has, which is the requested range intersected with what exists, so an
out-of-range bound is not a miss but a smaller intersection, and clamping
is the answer to the question that was asked. Framed that way the split
is not an asterisk but two coherent rules: **a point must exist; an
interval takes whatever part of it exists.**

Three reasons this is the right call for the audience, not just the
defensible one:

- **The truncation idiom is common and wants the clamp.** `s[1 to 20]` to
  show the first line of a description, `name[1 to 12]` to fit a label,
  `xs[1 to 3]` for the top few: game text is full of "up to N". If a
  slice failed when the value is shorter than the range, every one of
  these would need an `otherwise` or a length guard, on the exact
  operation whose appeal is that it does the bounds work for you. Making
  slices fallible is a net loss precisely where content creators live.
- **An empty result is a value, not an absence.** R9 floats "make an
  empty-result slice fallible". But `""` and `[]` are legitimate values,
  distinct from `nothing` (nil-nothing.md drew that line deliberately). A
  slice that intersects to nothing produced an empty value, it did not
  fail to produce one, and routing it through the failure channel would
  reconflate empty with absent, undoing that pass.
- **Clamp is the softer failure mode for this audience.** A slice near
  the end that silently returns fewer items shows a shorter list; a slice
  that traps ends the turn. For someone assembling room descriptions and
  inventories, the quietly-shorter result is almost always the kinder
  outcome, and the same reasoning that made a fault the right call for a
  clear bug makes clamping the right call for a benign edge.

**Close the gap with teaching, not behavior.** The learnability cost R9
names is real, so pay it down directly:

- **State the rationale where the policy lives.** fallible.md documents
  that indexing is fallible; add the point-versus-interval sentence next
  to it, so the split reads as designed rather than as an accident a
  reader has to reverse-engineer.
- **Make the confused error teach.** Today `s[a to b] otherwise d` and
  `if var x = s[a to b]` both get the generic "cannot fail / needs an
  expression that can fail". When the left is a slice specifically,
  replace that with a message that explains the split: a slice clamps to
  what exists and never fails, so it needs no `otherwise`; to notice a
  short or empty result, test `length` of the slice. This meets the builder
  with the right lesson at the one place they reach for the wrong tool.

**Strict "exactly this range or handle it" is a `length` check, not new
syntax.** When a builder genuinely needs "fail unless elements `a to b`
all exist", the spelling is a length test (`length(xs) >= b`) before or
after the slice, not a second slicing operator. A strict-slice construct
would add surface for a rare need and cut against the approachability
grain; `length` already works on both str and list results.

## Decisions

**D1. Keep slice clamping; do not make slices fallible.** Clamping is the
correct answer to interval access (intersect the range with what exists),
it preserves the common truncation idiom, and it keeps an empty result a
value rather than an absence. This is the whole behavioral content of the
pass: nothing changes at runtime.

**D2. Document the point-versus-interval rationale in fallible.md.** Add
the one framing sentence so the index-fails / slice-clamps split reads as
two coherent rules ("a point must exist; an interval takes whatever part
of it exists"), not a rule with an asterisk.

**D3. A slice-aware teaching error on a failure consumer.** When
`otherwise` or `if var` is applied to a slice (`n->a->kind == N_SLICE`),
replace the generic "the left cannot fail" with a message that a slice
clamps and never fails, and points to `length` for detecting a short or
empty result. This turns the surprise into a lesson at the point of
confusion.

**D4. No strict-slice construct; a `length` check spells "exactly N".** The
rare "fail unless the whole range exists" is a length test, not new
syntax. `length` already works on str and list slice results.
