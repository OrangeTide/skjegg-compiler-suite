# Consuming failure: `on fail`, and keeping the bool axis clear

Status: implemented (2026-07). A revision of can-fail.md's D2 (the
fallibility consumers), prompted by review: R14 stopped a `can fail` call
*typing* as bool, but left it being *consumed* by the boolean `if`, which
reuses a value-testing construct for a control-flow outcome. This note
separates the two axes and replaces the `if validate(k)` consumer with an
inline `on fail`. All seven decisions are confirmed and in the tree; see
the implementation notes at the end.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

## The problem R14 half-solved

R14 removed the *type* conflation: a `can fail` call no longer types as
`bool`, so a failure can no longer be stored, negated, or combined as a
value. But the R14 implementation kept the *consumer* conflation: `if
validate(k)` and `while validate(k)` still use the boolean `if` / `while`
to branch on a success/failure outcome. That is the Icon reflex (`if e
then ...` tests whether `e` succeeds) applied in a language Icon is not:
Excelsior has a real `bool`, and Icon does not.

Two axes now coexist that Icon never had at once:

- **bool** is a value: `true` / `false`, tested by `if` / `while`.
- **fallibility** is control flow: an expression *succeeds* (yields its
  value) or *fails*, consumed by `else`, `if var`, capture, a bare
  statement.

Overloading `if` to test both is a category slip, and it bites hardest at
the product of the axes. A `returns maybe bool` has **three** outcomes,
fail, true, false, and a single `if f()` expresses only two, so one pair
silently merges. Reusing the bool construct is exactly what lets false
and failure collapse into one branch.

A second problem surfaced with the `try` block first sketched for this:
`try { reserve(seat); charge(card) } on fail { alert() }` *reads* as a
transaction but is not one. If `reserve` succeeds and `charge` fails, the
handler runs with the seat already reserved and nothing rolled back. A
multi-statement failure block looks like an exception handler and does
not behave like one.

## What this revises

can-fail.md's D1 (a `can fail` call is `ET_SIGNAL`, not `bool`), D3
(reject it in value contexts), D4 (query-vs-signal guidance), and D5
(keep both kinds) stand. Only **D2** changes: the consumers. The
`ET_SIGNAL` type and the `no_signal` value rejections stay; the `if` /
`while` valueless consumer is removed and replaced.

## Survey

Every language that has both a boolean and a failure outcome keeps them
on separate constructs; only bool-less Icon merges them.

- **Icon.** No `bool`; `if e then ...` tests whether `e` succeeds, and
  comparisons like `a < b` succeed or fail. Icon collapses false into
  failure *by design*, because there is no false to collide with. It also
  drives generators with `every` (resume) versus `while` (re-evaluate),
  but Excelsior has no generators, so only the "loop while it succeeds"
  shape is relevant.
- **Rust.** `if` tests `bool`; `Result` / `Option` go through `match`,
  `if let Ok(x) =`, `?`, `unwrap_or`. `if` never tests a `Result`. A
  `Result<bool, E>` is `if let Ok(b) = ... { if b ... }`, the three
  outcomes explicit.
- **Zig.** The closest to the proposal here: `x catch |e| { ... }` and
  `x catch return` are an *inline* per-expression failure handler, `if
  (x) |v| else |e|` binds the value or the error, and plain `if` tests
  `bool`. Failure handling is a distinct operator, not the boolean `if`.
- **Swift / Go.** Swift's `do { try act() } catch { ... }` is
  block-scoped, and carries the transaction-illusion above; Go's `if err
  != nil` is per-site and explicit. Both keep failure off the boolean
  `if`.

The consensus is uniform and matches the review: `if` / `while` test
values; failure gets its own consumer. Zig's inline `catch` is the
precedent for making that consumer a per-statement suffix rather than a
block.

## Design

**The principle: `if` and `while` test bools. Failure has its own
consumers.** A plain `if` / `while` condition must be a `bool`; a
fallible (`maybe`-typed or `ET_SIGNAL`) condition is rejected, so failure
can never ride the boolean construct and collapse with false.

The consumers of failure, each on its own axis:

- **`else` — value fallback** (unchanged). `expr else default` yields a
  value; the fallback replaces a failed value. Expression level, never
  bool. `xs[i] else 0`.
- **`if var` / `while var` — value-fallible binders** (unchanged). Bind
  the payload on success, take the else-branch (or exit the loop) on
  failure. This is where "handle a failure and a bool at once" lives: the
  function is `returns maybe bool`, and

      if var ok = check()        -- check() returns maybe bool
          if ok ... else ...     -- succeeded: true vs false
          endif
      else
          ...                    -- failed
      endif

  Three outcomes, no collapse. Because plain `if check()` is now
  rejected, the checker *forces* the failure to be handled before the
  bool is tested.
- **`on fail` — statement-level failure handler** (new). The
  statement-level twin of `else`:

      reserve(seat) on fail alert()

      stmt  =  simple_stmt [ "on" "fail" simple_stmt ] NL

  Evaluate the left statement; on success continue past the handler; on
  failure run the handler statement (which consumes the failure), then
  continue, unless the handler transfers control (`return`, `break`,
  `continue`, `fail`). The left must contain a fallible producer, or the
  `on fail` is dead and rejected (as `else` rejects an infallible left).
  The keyword is `on fail`, not `else`: it reuses the language's `on
  <event>` idiom, and a distinct word keeps the failure axis visibly off
  the boolean `else`.

`else` and `on fail` are the same idea at two levels, "on failure of the
left, the right", one value-typed, one an action. Neither touches bool.

**No `try` block; failure is handled per action.** Dropping the block
removes the transaction-illusion and, in practice, reads better: each
fallible action carries the handler that fits it.

    reserve(seat) on fail alert()
    charge(card)  on fail refund()

The "do X on success, Y on failure" case is early-return:

    reserve(seat) on fail return
    confirm()                       -- success path

If a shared handler over a real group is ever wanted, the right tool is a
*generic* compound-statement block that `on fail` composes with (`<block>
on fail H`), not a failure-specific `try`. That is deferred: it cuts
against Excelsior's `endX`-per-construct style and deserves its own
justification, and per-action handlers cover the cases without it.

**The valueless loop composes; it needs no construct.** "Loop while a
valueless action succeeds" (Icon's `while <expr>`) is built from the two
clean primitives, `while true` (already a tested idiom) and `on fail
break`:

    while true
        step() on fail break        -- loop until step() fails
    endwhile

Work per successful step, or a count, follows in the body after the `on
fail break`. This keeps `while` bool-only, consistent with removing `if
<can fail>`, and pays two keywords for a genuinely rare, systems-flavored
pattern rather than reintroducing the axis overloading. (Most loops that
look valueless want a value, `while var x = pop()`, or a collection, `for
x in xs`.) A `repeat ... endrepeat` sugar for `while true` is a possible
later nicety, decided on its own merits, not folded in here.

## What this reverses from the committed R14

- Remove the `want_bool`-accepts-`ET_SIGNAL` change: `if validate(k)` and
  `while validate(k)` no longer consume a signal.
- Reject a fallible (`maybe` or `ET_SIGNAL`) condition in a plain `if` /
  `while`, pointing to `if var` / `on fail`.
- `else`-on-signal stays rejected; its message points to `on fail`.
- `tests/exs_maybe.exs`'s `if validate(5)` migrates to `validate(5) on
  fail ...`; `validate`, a pure `n < 0` check, is really a `returns bool`
  predicate (the D4 re-spelling), so the migration also fixes the
  example.
- `ET_SIGNAL` and the `no_signal` value rejections are unchanged.

## Staging

1. **Parser.** Add the `on fail` statement suffix: `simple_stmt [ "on"
   "fail" simple_stmt ]`. `on` need not be a global keyword if `on fail`
   is recognized as a suffix at statement end; `fail` already is one.
2. **Checker.** Remove the signal case from `want_bool` and reject a
   fallible condition in plain `if` / `while`. Add the `on fail` rule:
   the left must be fallible (a `can fail` call or a statement with a
   fallible producer), the handler is any statement.
3. **Lowering.** `on fail` sets the dynamically-scoped fail label to a
   handler label around the left statement (the same mechanism `else`
   uses); a `can fail` call's zero success-word branches there. Success
   falls through past the handler.
4. **Migrate** `exs_maybe.exs` and add an `on fail` test.

## Decisions (confirmed and implemented)

**D1. `if` / `while` test bools only; reject a fallible condition.** A
`maybe`-typed or `ET_SIGNAL` condition in a plain `if` / `while` is a
compile error pointing to `if var` / `on fail`. Failure can never ride
the boolean construct, so false and failure cannot collapse.

**D2. Inline `on fail` as the statement-level failure handler.** `stmt on
fail handler`, the twin of the value-level `else`, is the consumer for a
valueless `can fail` action and for any fallible statement. It replaces
the removed `if validate(k)` consumer.

**D3. Handler is a single statement.** Keep the inline form one line;
factor a longer handler into a func (`... on fail cleanup()`). The
deferred generic block is the future path to a multi-statement handler.

**D4. `on fail` attaches to any fallible statement.** Not only a bare
`can fail` call: `x = xs[i] on fail x = 0` and a discarded value-fallible
are covered too. The left must contain a fallible producer.

**D5. No `try` block; generic-block grouping deferred.** Per-action
handlers avoid the transaction-illusion and read better; a generic
compound-statement block that composes with `on fail` waits for its own
justification.

**D6. The valueless loop composes from `while true` + `on fail break`.**
No dedicated valueless-`while` consumer and no new loop construct; a
`repeat` sugar for `while true` is a separate, later question.

**D7. `else` stays value-only.** `else` is the expression-level value
fallback; it does not apply to a valueless `can fail` action, which uses
`on fail`. `else`-on-signal is rejected with a message pointing there.

## Implementation notes

Across the four pipeline stages, the runtime unchanged (a `can fail` func
already returns the 0/1 success word; every fallible producer already
branches to the dynamically scoped fail label):

- **Parser** (`parse.c`). A new `N_ONFAIL` node (`a` = left statement,
  `b` = handler) wraps an assignment or expression statement when the
  contextual `on fail` suffix follows. `on` is recognized by two-token
  lookahead (`at_on_fail`: a bare identifier `on` then the `fail`
  keyword), so it never becomes a reserved word. The handler is parsed by
  the ordinary `parse_stmt`, so `return`, `break`, `continue`, and a bare
  call all work and the handler consumes its own newline.
- **Checker** (`typecheck.c`). `want_bool` now rejects a fallible
  condition (an `ET_SIGNAL` with the `on fail` hint, a `maybe` with the
  `if var` / `else` hint), so `if` / `while` are bool-only. The `if var`
  / `while var` signal message and the `else`-on-signal message now point
  to `on fail`. `N_ONFAIL` checks the left statement, requires its
  principal producer to be fallible (`maybe`, `ET_SIGNAL`, or `any`), and
  checks the handler as any statement.
- **Lowering** (`lower.c`). `N_ONFAIL` lowers the left inline (not through
  `lower_stmt`, which resets `cur_fail` per statement) so `cur_fail` set
  to a handler label reaches the producer: a bare `can fail` call's zero
  word branches there via `fail_if_zero`, and an assignment's fallible
  RHS branches there before the store. Success falls through and skips
  past the handler; the handler is lowered normally and may transfer
  control (`break` in the valueless-loop idiom, `return` in the guard).
- **Resolver** (`resolve.c`). `N_ONFAIL` resolves both statements in the
  enclosing scope.

**D1's reach, discovered in implementation.** Rejecting a fallible
condition is broader than the `can fail` signal: any `maybe`-typed
condition is now rejected too, which the old `want_bool` had silently
accepted by stripping the `maybe`. Surfacing that made visible a separate
over-approximation: every int/decimal `/` and `%` was typed fallible even
when the divisor is a nonzero constant and the operation provably cannot
fail. That is fixed at its source (numbers.md): a nonzero literal divisor
is infallible, a literal-zero divisor is a compile error. With that in
place, `if a / 2 == 3`, `if a % 2 == 1`, and `if n % 2 == 1` type as
plain bool again and need no change; only a genuine fallible condition
survives. The one real D1 tax is `if s[1] == "h"` in `exs_str_ops.exs`: a
string index can fail (the length is unknown at compile time), so it is
migrated by hoisting the value into a plain var first (`var c is str =
s[1]` then `if c == "h"`; inference never captures, so an out-of-range
index traps, exactly the prior runtime behavior). Slices clamp and stay
infallible. `exs_maybe.exs`'s `if validate(5)` / `if validate(-1)` became
`validate(5) on fail ...`. A new `exs_on_fail.exs` covers the
success-skips-handler, index recovery on an assignment, the `while true`
+ `on fail break` valueless loop, and the early-return guard. check-exc
46/46.
