# Exploring `else` as a general fallback operator

Status: design exploration (2026-07); RESOLVED by fallible.md later the
same month (maybe T, Icon propagation, the four consumers), and the v1
is implemented. Kept for the rationale trail. The `select idx from ... else d`
operator has shipped with `else` baked into its grammar. This note explores
generalizing `else` into a fallback operator usable beyond `select`, what
"triggers" it, which types it covers, and a recommended path.

Note (2026-07): the fallback operator spelled `A else B` throughout this
note was later renamed to `A otherwise B` (fallback-words.md), and
`select` was itself retired (choosers.md). Read every `A else B` below
as `A otherwise B`. The sources/continuation-pump sketch that closed
this note has been extracted to sources.md and decided there; nothing
of it is implemented yet.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)


## The idea

`A else B` yields `A` when `A` produced a value, otherwise `B`. It reads as a
natural-language fallback and composes with anything that can fail to produce
a value. `B` is evaluated only when needed (lazy). Chaining is right
associative: `A else B else C` is `A else (B else C)`, first value wins.

Two examples already in reach:

    select idx from "red", "green", "blue" else "unknown"   // shipped
    s[i] else "?"                                            // string index


## The core question: what triggers the `else`?

There are two distinct notions of "no value", and they cover different types.

### Trigger 1: nil (nil-coalescing)

`A else B` = `(A is nil) ? B : A`. This is the familiar `??` operator.

- Works for the **reference types** where `nil` is a legal value. In the
  current checker `nil` is assignable to `is_ref` types (obj, and str / list
  when treated as references).
- Does **not** cover the value types (int, float, bool, fixed): they have no
  nil, so there is nothing for `else` to catch. `pts else 0` is meaningless
  when `pts` is a plain int that is always some value.

### Trigger 2: out-of-range / failure of a fallible operation

The left operand is not a value but an *operation that can fail*: an index
(`arr[i]`) or a `select`. The operation's bounds check itself picks `B` when
the index is out of range. No value sentinel is involved.

- Works for **any** type, value or reference: the default is just the branch
  the operation takes when it has no in-range result.
- This is exactly how `select ... else d` lowers today: the out-of-range path
  stores `d` into the result slot. Nothing is ever nil.

The two triggers are not the same mechanism. Nil-coalescing inspects a
produced value; the fallible-operation form is a branch inside the operation
before any value exists. A value-typed `select` cannot use nil-coalescing,
and a plain nilable value has no bounds to be out of.


## Recommended model: `else` dispatches on the left operand's kind

Keep one surface operator, `A else B`, and let the compiler choose the
lowering from what `A` is, statically:

1. **`A` is a fallible operation** (`select ...`, an index `x[i]`): `else`
   is that operation's out-of-range branch. Covers all types. This is what
   `select` already does; extend the same treatment to indexing. No runtime
   sentinel, no nil.

2. **`A` is a nilable value** (a reference-typed expression): `else` is
   nil-coalescing. Evaluate `A` once (spill to a slot), compare to nil, yield
   `A` or `B`. Reference types only.

Both read identically and compose; the difference is invisible to the writer.
`B` must be assignable to `A`'s (non-nil) type; the result is that type.

Precedence: `else` binds looser than comparison and the postfix operators, so
`arr[i] else d` is `(arr[i]) else d` and `a < b else c` is `(a < b) else c`.
Right associative for chaining.


## What each step needs

- **`select ... else` — done.** The first instance. Its `else` is already the
  fallible-operation form (case 1), which is why it handles value types.
- **String index `s[i] else d` — done.** `else` is now a real operator: the
  lowest-precedence, right-associative `A else B` (`N_ELSE`). For a string
  index left operand the lowering does the bounds test inline (1 <= i <=
  length(s), reading the descriptor's length word) and branches to `s[i]` or to
  `d`; `d` is evaluated only when out of range (lazy). `select` parses its
  branches one level above `else` (parse_or_level), so its own trailing
  `else` is not swallowed. Other `else` left operands still error as
  not-lowered.
- **List index `xs[i] else d` — done.** A homogeneous data literal
  (`[1 2 3]` or `["a" "b"]`) lowers to a constant list global
  `{ count, e0, e1, ... }` (int/bool words or interned string pointers); a
  list value is that pointer, like a str. Since the count sits in word 0
  just like a string descriptor's length, the `else` bounds-check skeleton
  is shared with the string case; only the in-range load differs
  (`*(list + i*4)` versus `__exc_str_at`). Plain `xs[i]` is unchecked;
  `xs[i] else d` is the safe form. Only constant list literals are
  constructible so far (list build/append/set operations are future work).
- **Nil-coalescing `A else d`.** Needs (a) a clear notion of which expressions
  are nilable, and ideally a nilable-type spelling (the grammar has no `str?`
  today; `nil` is just assignable to reference types), and (b) `nil` lowered
  (it is currently "not lowered yet"). Until `nil` lowers, case 2 cannot be
  built.


## Open decisions before implementing the general operator

1. **Fold `select`'s `else` into the general operator, or leave it baked in?**
   The syntax `select ... else d` is identical either way. Folding means the
   parser produces `else(select(...), d)` and the select node carries no
   default; the `else` lowering special-cases a `select`/index left operand.
   This is cleaner long term but only pays off once a second fallible
   construct (indexing) exists. Recommend: leave `select`'s `else` baked in
   until indexing-with-`else` lands, then refactor both onto one `N_ELSE`.

2. **Does `else` cover nil-coalescing at all, or only fallible operations?**
   If the language leans on `nil` for absence (host lookups, optional fields),
   nil-coalescing is valuable and worth building once `nil` lowers. If absence
   is always expressed through fallible operations, case 2 can be dropped and
   `else` stays a modifier on `select`/index only. This depends on how the
   host ABI models optional results (see host-abi.md).

3. **Out-of-range policy for a bare fallible op (no `else`).** `select` traps
   today. An index with no `else` (`s[i]`) would either trap or keep its
   current clamp/empty behavior. Recommend trap for consistency once `else`
   is the sanctioned way to provide a default.


## Recommendation

Ship order: (1) `select ... else` — done. (2) Extend `else` to string/list
indexing as a fallible-operation default, refactoring `select` and index onto
a shared `N_ELSE` at that point. (3) Add nil-coalescing `A else B` only after
`nil` lowers and the host ABI settles how optional results are represented.
Steps 2 and 3 are independent; do whichever the content needs first.


## A second consumer: `if var` binding (added 2026-07, Icon input)

Icon's `if i := find("or", line) then write(i)` is not C-style
assignment-in-condition; Icon's `if` tests success, not truth, and the
binding rides along. That success/failure model is exactly this note's
fallible expression, which makes the binding form the natural second
consumer of the concept, next to the inline `else` fallback:

    var i = find("or", line) else 0     // expression form: fallback value
    if var i = find("or", line)         // statement form: branch on success
        write(i)
    endif

Syntax costs nothing new: `if` + `var` + `=` with existing meanings, the
bound name scoped to the then-block, `elseif var` chaining, LL(1) on the
`var` after `if`. The C footgun (`=` as an expression) and a new `:=`
token are both avoided.

Gated on the same open question as nil-coalescing above: it needs
expressions that can *declare* fallibility (a `find` whose type is
"position or nothing") rather than trapping, so it lands with the
nilable/fallible-type design, not before. When that design happens,
`if var` is an argument for doing it well: one concept (a value or not),
two consumers (`else` for the default, `if var` for the branch).


## Sources: `while var`, `for in`, and the generator question (2026-07)

The sketch that lived here (the fallible pump as the iteration protocol,
sources as second-class turn-local values, the cursor-record and
failable-continuation backings) moved to its own note, **sources.md**,
and was decided there (2026-07). What stays for the
rationale trail is the conclusion: consumers of fallibility number four,
`otherwise` for the default, `if var` for the branch, `while var` for
the pump loop, and `for in` as sugar over the pump.
