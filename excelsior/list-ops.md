# List operations: composing and decomposing a value list

Status: decided (2026-07), implemented (D1 to D4, D6, D7): the composition
builtins (`prepend`/`insert`/`reverse` beside `append`/`set`/`delete`), `+`
concat, the `first`/`last`/`rest` decomposition, and the `len` to `length`
rename all ship (`tests/exs_list_compose.exs`). D5's higher-order combinators
(`map`/`filter`/`reduce`, comparator `sort`) belong to function-values.md and
are not yet implemented. A collections pass, filling out the operation set on the
copy-on-write `list of T` value. Tier: World (tier 1); building and composing
lists is everyday content work. Builds on the shipped list surface (`length`,
`append`, `set`, `delete`, indexing, slicing, `in`, `find`, `for`), the
CoW value semantics (buffer.md), the point-versus-interval access rule
(slice-clamp.md), and the string-concat precedent (string-plan.md).

The list already has the primitives, an indexed read, a length, and the three
CoW mutators `append` / `set` / `delete`. What it lacks is a **friendly,
complete composition set**: adding to the front, joining two lists, inserting in
the middle, taking the ends, reversing. This pass settles that set, and draws
the line between the value list (immutable composition, value-returning) and the
buffer (mutation-heavy stack and queue work, buffer.md), so each operation lands
where it belongs.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

## The shape of the answer: two containers, one line between them

Every list operation is either a **value composition** (produce a new list from
old ones) or an **in-place mutation** (change a container repeatedly). The
language already has a home for each:

- the `list of T` **value** is copy-on-write, so every operation on it *returns a
  fresh list* and leaves the original alone (buffer.md); it is the composition
  container.
- the `buffer of T` **transient** mutates in place with amortized O(1) `push` /
  `pop`; it is the accumulation container (buffer.md).

So the design rule for where an operation goes is simple: **a value-returning
composition is a list operation; a repeated in-place mutation is a buffer
operation.** This is what keeps a hidden O(N^2) off the list. Calling
`append(xs, x)` in a loop and rebinding `xs` is quadratic (each `append` copies);
the loop that needs to grow uses a `buffer` and freezes once with `tolist`
(buffer.md). The list operations below are for composing a handful of lists, not
for growing one element at a time in a hot loop.

## Composing: build a new list from old parts

All of these return a fresh CoW list, like the shipped `append` / `set` /
`delete`:

- **`append(xs, x)`** adds `x` to the end (shipped).
- **`prepend(xs, x)`** adds `x` to the front, the symmetric partner `append`
  was missing.
- **`insert(xs, i, x)`** adds `x` at position `i` (1-based), shifting the rest
  right; `i` past the end appends.
- **`set(xs, i, x)`** replaces the element at `i` (shipped).
- **`delete(xs, i)`** removes the element at `i` (shipped).
- **`reverse(xs)`** returns the elements in reverse order.

Adding *one element* at an end is a word (`append` / `prepend`); adding at a
position takes an index (`insert`). None of these is `+`, because `+` joins two
*lists*, and keeping the element form a word avoids the `xs + x` ambiguity (is
`x` an element or a one-element list?).

## Concatenation is `+`, the string parallel

Joining two lists is **`+`**, exactly the operator that already concatenates two
strings (string-plan.md):

    total = front + back              // two lists joined into one
    path  = [start] + middle + [end]  // singletons and a list, all joined

There is no separate `concat` or `join` word. A string is a sequence and a list
is a sequence, so `+` is the single rule "join two sequences into one", learned
once and read everywhere. This is the one place a symbol does what looks like
structure, and it earns the exception by *already being how strings concatenate*:
introducing a `concat` word for lists would make the two sequence types diverge
for no gain. `xs += ys` is the rebind sugar (`xs = xs + ys`), matching the
numeric and set `+=` (set-of.md); like every list composition it produces a fresh
value, so in a growth loop it is still the buffer's job.

## Decomposing: take the ends, reusing point-versus-interval

Reading pieces back out follows the access rule slice-clamp.md already set, a
**point must exist, an interval takes whatever part of it exists**:

- **`first(xs)`** and **`last(xs)`** are point access: they name one element that
  must be there, so on an empty list they *fail* (fallible, consumed by
  `otherwise` / `if var`, like `xs[i]`):

      show(first(names) otherwise "nobody")

- **`rest(xs)`** is interval access, all elements after the first: on an empty
  or one-element list it clamps to the empty list `[]`, no failure, exactly as a
  slice does.

The shipped `length(xs)`, indexed `xs[i]` (fallible point), sliced `xs[a to b]`
(clamped interval), `x in xs`, and `find` are unchanged; emptiness is `length(xs)
== 0` (no new word). `first` / `last` / `rest` are the friendly names for the
ends so an author does not compute `xs[length(xs)]` by hand.

## Stacks and queues: the "pushdown" is a buffer

A pushdown stack, or a queue, is *repeated* push-and-pop, which is precisely the
in-place mutation the value list must not carry (a `pop` that mutates-and-returns
on a CoW list is an O(N^2) trap dressed as a convenience). So there is **no
`push` / `pop` on the value list**; a real stack or queue is a **`buffer`**
(buffer.md), whose `push` / `pop` are in-place and amortized O(1), spelled
differently from the list's value-returning `append` on purpose.

The value list still expresses a stack *functionally*, when you want a new list
rather than a mutated one: `prepend` / `first` / `rest` are the classic cons
triad (push onto the front, read the top, drop the top), each returning a fresh
list. That is the immutable stack; the buffer is the mutable one. Naming the two
clearly is the friendliness: the author reaches for a buffer when they are
looping, and for the list words when they are composing.

## What is deferred

- **Higher-order combinators** (`map`, `filter`, `reduce` / `fold`). They need a
  function-value or block design the language does not yet have, and the
  audience-friendly idiom today is an explicit `for` loop that builds a buffer,
  which reads more plainly for a non-programmer than a folded lambda. They wait
  on a lambda/block pass.
- **`take(xs, n)` / `drop(xs, n)`** (a prefix or suffix) and **all-but-last**.
  Slicing (`xs[1 to n]`, `xs[2 to length(xs)]`) already covers these, so they are
  conveniences to add if the slice spellings prove awkward, not core.
- **`sort`** needs an ordering or a comparator, which is a design of its own
  (a key function or a `less` predicate), deferred with the higher-order work.
- **In-place mutation of a uniquely-owned list** (an optimization). The
  mutators are copy-on-write, so `prepend`/`insert` (and the rest) allocate and
  copy. When the memory.md refcounting lands, a mutator whose input is uniquely
  referenced (refcount 1) can mutate in place instead, the
  isKnownUniquelyReferenced / transient trick, keeping the value semantics while
  dropping the copy. It is gated on refcounting (the arena has none today), so
  every mutator copies for now; `prepend`/`insert` benefit most.

## Survey

- **Clojure**: `conj` / `cons` / `concat` / `first` / `rest`, persistent values
  with a `transient` for accumulation, the exact value-plus-transient split this
  pass uses (buffer.md's `tolist` is `persistent!`). Excelsior spells concat `+`
  and keeps `first` / `rest`.
- **Python**: `list + list`, `.append`, `.insert`, `.pop`, slicing; the `+` for
  concat and index/slice access are the shared vocabulary, but Python's list is
  mutable, so its `append` / `pop` are Excelsior's *buffer* operations, not
  list ones.
- **Lisp**: `first` / `rest` (over `car` / `cdr`), `cons`, `append`, `reverse`,
  the readable-name lineage for the decomposition triad.
- **Haskell**: `++`, `head` / `tail` / `init` / `last`, `map` / `filter` /
  `foldr`; Excelsior takes the value semantics and defers the higher-order
  combinators, and prefers the plainer `first` / `rest` to `head` / `tail`.
- **JavaScript**: `push` / `pop` / `shift` / `unshift` / `concat` / `slice`, a
  mutable-array grab-bag; Excelsior sorts the mutating half onto the buffer and
  the value half onto the list rather than piling both onto one type.

Excelsior's stance: a complete value-returning composition set on the CoW list
(`append` / `prepend` / `insert` / `set` / `delete` / `reverse`), `+` for
sequence concatenation shared with strings, `first` / `last` / `rest` decomposition
under the point-versus-interval rule, and stack/queue mutation kept on the buffer
where the cost is honest.

## Decisions (confirmed)

The seven decisions are confirmed. The implementation (the new `prepend` /
`insert` / `reverse` / `first` / `last` / `rest` builtins over the CoW list, `+`
list concatenation beside the existing string concat, the fallible-versus-clamp
split on the decomposers, and the `len` to `length` rename with its
migration hint) follows; stack/queue mutation stays with buffer.md, and the
higher-order combinators wait on the lambda/block backlog item.

**D1. The value list gets a complete value-returning composition set:**
`append` (shipped), `prepend`, `insert(xs, i, x)`, `set` (shipped), `delete`
(shipped), and `reverse`, all copy-on-write returning a fresh arena list.
Adding one element at an end is a word (`append` / `prepend`); adding at a
position takes an index (`insert`).

**D2. List concatenation is `+` (with `+=` rebind sugar), the same operator as
string concat.** A string and a list are both sequences, so `+` is the one rule
"join two sequences"; there is no separate `concat` / `join` word. The
words-do-structure tension is resolved by the shipped string precedent, since a
list-only concat word would make the two sequence types diverge for no gain.

**D3. Decomposition reuses slice-clamp.md's point-versus-interval rule.**
`first(xs)` / `last(xs)` are fallible point access (the element must exist,
consumed by `otherwise` / `if var`), `rest(xs)` is interval access that clamps to
`[]` on an empty or singleton list; `length`, `xs[i]`, `xs[a to b]`, `in`, `find`
are unchanged and emptiness is `length(xs) == 0`.

**D4. Repeated push/pop (a stack or queue, the "pushdown") is the buffer, not the
value list.** There is no `push` / `pop` on the CoW list, because a
mutate-and-return on a value is an O(N^2) trap; `buffer` carries the in-place
amortized-O(1) `push` / `pop` (buffer.md). The functional stack on a value list
is the `prepend` / `first` / `rest` cons triad.

**D5. Higher-order combinators and sort are deferred.** `map` / `filter` /
`reduce` need a function-value/block design, and the friendly idiom today is a
`for` loop building a buffer; `sort` needs a comparator. `take` / `drop` / an
all-but-last word are slice-covered conveniences, added only if the slice
spellings prove awkward.

**D6. New names are bare run-together words** (`prepend`, `insert`, `reverse`,
`first`, `last`, `rest`) per the `length` / `spawn` / `append` convention, no
underscores. The positional replace stays `set` (unambiguous against the `set of`
type, which is a type position), with `replace` recorded as the clearer
alternative should the shared word prove confusing.

**D7. The length builtin is `length`, not `len`.** A plain English word reads
better for a new user than an abbreviation, and it is a better example of the
bare-name convention than a truncated one. This is a spec-wide surface rename
(the checker answers a stray `len(...)` call with a migration hint, like the
de-arrow hints; `len` stays an ordinary identifier to the lexer, so it is
still usable as a name); the
internal `{ len, data }` string descriptor field and the `__exc_str_len` host
helper keep their C names, since the rename is of the author-facing surface, not
the implementation.
