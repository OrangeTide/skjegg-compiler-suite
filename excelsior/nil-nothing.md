# Two absence words: teaching `nil` from `nothing`

Status: decided and implemented (2026-07), the pass on
approachability.md's R6 (two absence words). All four decisions at the
end were confirmed and are in the tree: the two-word design is kept, the
`nothing`-into-a-ref soundness hole is closed, and both mix-up directions
now report a teaching error at the six value-to-slot boundaries.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

## The problem

Excelsior has two words for absence. `nothing` is "no result", the
absence literal for a `maybe T` (fallible.md). `nil` is "no object", an
absent reference. The split is principled: a successful `nil` and a
`nothing` would be indistinguishable at runtime, which is exactly why
`maybe obj` is forbidden. But a beginner meets both words and must learn
which context wants which, and R6 observes the mitigation is entirely
error-message quality.

Measuring the current messages showed the gap is worse than a wording
polish, in both directions:

- **`nil` where a result is wanted** produced a generic mismatch:
  `cannot initialize \`x\` (maybe int) from nil`. It names the types but
  not the fix (`nothing`).
- **`nothing` where an object is wanted was silently accepted.**
  `var o is obj = nothing` and `return nothing` from a `returns obj`
  type-checked clean. `nothing` is a `maybe any`, and `assignable`
  strips a maybe to its payload (`any`), which stores anywhere. So the
  one direction that should be loudest was silent. That is a soundness
  hole, not only a message gap: a "no result" value flows into a plain
  object slot and becomes an untracked null.

So R6 is two teaching errors *and* one correctness fix.

## What exists today

- `nil` is `N_NIL`, typed `ET_NIL`; `nothing` is `N_NOTHING`, typed
  `ty_nothing = maybe_of(ty_any)` (typecheck.c). `assignable` lets `nil`
  into any ref type (`is_ref`: obj, err, prop, list, a named type, nil),
  and lets a `maybe` (including `nothing`) into a slot by comparing
  payloads, which is what silently admits `nothing` into a non-maybe.
- The type spelling is already guarded: `maybe obj` is a parse error with
  a teaching message ("an absent object is `nil`"), and a `maybe` field
  must default to `nothing` (a checker rule). Those cover the *type*
  side; R6 is about the *value* literals meeting a slot.
- The value-to-slot boundaries are six: a call argument, a `var`
  initializer, an assignment, a `return`, a field default, and a `const`
  initializer. Each computes the source type, checks `assignable`, then
  `widen_to`. Comparisons (`x == nil`) are deliberately not boundaries;
  testing a maybe or an obj against `nil` is legitimate (fallible.md).

## Survey

The two-absence design is the interesting axis. Prior art splits three
ways:

- **One null for everything** (C `NULL`, Java `null`, Go `nil`). Hoare's
  "billion dollar mistake": one untyped absence that inhabits every
  reference type, unchecked. Excelsior rejects this; `nil` is confined to
  references and `nothing` carries result-absence in the type.
- **One option type, no separate null** (ML `option`, Haskell `Maybe`,
  Rust `Option`, Swift `Optional`). Absence is always `None`/`Nothing`
  inside a typed wrapper; there is no second bare word. Clean, but it
  makes every absent reference a wrapped value, which Excelsior avoids by
  giving references their own in-band `nil` (an obj is never at address
  zero, so `nil` costs nothing).
- **Two absences, distinguished** (JavaScript `null` vs `undefined`).
  The direct precedent for "two words", and the cautionary one: the
  distinction is real but under-taught, so it is a perennial beginner
  trap and style-guide battleground. The lesson is not "don't have two",
  it is "if you have two, the tooling must teach the boundary relentlessly,
  because the words alone will not." That is precisely R6's
  recommendation.

On the remedy, the precedent is the one this series already leans on:
where a distinction is principled but easy to confuse, the error message
is the product surface (Elm, Rust; and Excelsior's own removed-syntax
migration hints and the R4/R5 teaching passes). A confusion that the
checker can detect and name is a confusion the beginner does not have to
carry.

## Design

**Close the soundness hole and teach both directions with one helper.**
At each of the six value-to-slot boundaries, before the generic
`assignable` check, run a small `absence_mixup(src_node, dst_type)`:

- `src` is the literal `nothing` and `dst` is not a `maybe` (and not
  `any`/`prop`): reject. If `dst` is a reference type, name `nil`
  ("`nothing` is an absent result; for an absent obj use `nil`"); if it
  is a value type, name the fix as making the slot a maybe ("store it in
  a `maybe int`, not a plain int"). This is the correctness fix: a bare
  `nothing` can no longer reach a non-maybe slot.
- `src` is the literal `nil` and `dst` is a `maybe`: reject, naming
  `nothing` ("`nil` is an absent object; for an absent result write
  `nothing` (this slot is `maybe int`)").

Running before `assignable` means the `nil` case pre-empts the generic
mismatch message, and the `nothing` case fires where `assignable` would
otherwise have waved it through. The valid pairings (`nothing` into a
maybe, `nil` into a ref) fall through untouched, as does `any`/`prop`.

The check is confined to the literal words. A general `maybe`-to-payload
flow (a computed `maybe int` value, not the bare `nothing` literal) is
untouched, so the fallibility rules (capture, unwrap, the NF_FALLIBLE
consumer checks) are unchanged. Comparisons are not boundaries, so
`if key == nil` and testing a maybe for `nothing` still work.

**The principle.** An absence literal that meets the wrong slot names the
right word. The two-word design (D1) is kept exactly as fallible.md set
it; the whole pass is the tooling the survey says a two-absence language
must have.

## Staging

One change, two independent payoffs, landing together:

1. The `absence_mixup` helper and its six call sites (typecheck.c).
   Closes the `nothing`-into-a-ref hole and adds both teaching errors.

There is no positive test artifact for an error path (the harness runs
no expected-compile-failure cases); the six directions were verified by
hand and the full suite confirms no valid program regressed.

## Decisions (confirmed and implemented)

**D1. Keep the two-word design.** `nothing` for result absence, `nil`
for object absence, `maybe obj` forbidden. R6 endorses the design; this
pass changes only the tooling around it.

**D2. Reject `nothing` in a non-maybe slot.** Close the soundness hole so
a "no result" literal cannot silently become an untracked null in an
object or value slot. This is a correctness fix, not only a message; it
did not regress any existing program.

**D3. Teaching errors in both directions.** At the value-to-slot
boundaries, `nil` into a maybe names `nothing`, and `nothing` into a
non-maybe names `nil` (for a ref) or the maybe slot (for a value), in the
removed-syntax hint spirit R6 asks for.

**D4. Scope to the six value-to-slot boundaries.** Apply the check at
argument, `var`, assignment, `return`, field-default, and `const`
boundaries, and only to the bare `nil`/`nothing` literals. Leave
comparisons alone (`== nil` is legitimate) and leave general maybe flow
to the existing fallibility rules.

## Implementation notes

All in `typecheck.c`. The `absence_mixup(src, dst)` helper sits just
after `assignable`; it fires only for the `N_NOTHING` and `N_NIL`
literals and is a no-op otherwise. It is called at the six boundaries
(check_args, `N_VAR`, `N_ASSIGN`, `N_RETURN`, the field default in
check_class, and the `N_CONST` initializer), each time before that
site's `assignable` check, so the `nil` case pre-empts the generic
mismatch and the `nothing` case fires where `assignable` would have
admitted it. The messages, verified by hand across all six directions:

    `nil` is an absent object; for an absent result write `nothing`
    (this slot is `maybe int`)

    `nothing` is an absent result; for an absent obj use `nil`
    (`nothing` needs a `maybe T` slot)

    `nothing` is an absent result; store it in a `maybe int`, not a
    plain int

The valid pairings (`nothing` into a maybe, `nil` into a ref) and the
`any`/`prop` wildcards fall through untouched; the full suite confirmed
no valid program regressed when the `nothing`-into-a-ref hole closed.
