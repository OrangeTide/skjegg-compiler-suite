# `buffer of T`: the mutable builder behind the immutable value

Status: decided (2026-07), not yet implemented. A pass from the backlog (backlog.md, TODO's growable-buffer
item). Tier: Mechanics (tier 2); a buffer is a performance and construction tool
for the systems author, its gentlest use (building a string in a loop) named in
the mechanics guide, not the tutorial (tiers.md). Interacts with the list
(records.md's copy-on-write value), strings (string-repr.md's owned buffer), and
the arena memory model.

A `list of T` is a copy-on-write **value**: a mutator returns a fresh list and the
old one is untouched (CLAUDE.md). That is the right default for stored, shared,
sent data, but it makes incremental construction quadratic: appending N items
one at a time copies the growing list N times, O(N squared). A `buffer of T` is
the missing counterpart, a **mutable, growable** container with amortized O(1)
append, that you build into and then **freeze** into an immutable value. It is
the builder; the list and the string are what it produces.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

## The value / mutable duality

The language's composite types are **values**: an int, a decimal, a str, a
record, a list, and a set are all copied on assignment and have no identity
(records.md, set-of.md). Values are safe to store, share, and send, because
nothing can change one under you. An **actor** (an object) is the one reference
type, with identity and dispatch, reached across the send boundary.

A growable buffer is neither. Amortized O(1) append **requires** in-place
mutation of a shared backing store, which value semantics forbid (a copied value
cannot share a backing) and which the actor boundary must not carry (shared
mutable state across actors is the thing the model excludes). So a buffer is a
third, deliberately narrow kind: a **mutable, identity-bearing, actor-local
scratch container** that never crosses a send and never persists on its own.
You build into it, then freeze it into a value; the value is what escapes.

This is exactly Clojure's persistent-vector / transient split: a persistent
vector is the immutable value, a transient is a mutable local you `conj!` into
and then `persistent!` back into a value. Excelsior's list is the persistent
value, `buffer of T` is the transient, and `tolist` is `persistent!`. It is also
string-repr.md's story for strings: an owned string is "born from a builder",
and `buffer of char` is that builder made first-class.

## The design

### `buffer of T`, mutable and growable

`buffer of T` is a mutable, contiguous, growable sequence of word-sized `T`, with
a **length** (how many elements it holds) and a **capacity** (how many it can
hold before it grows). Appending past the capacity grows the backing store
(doubling), so a run of appends is amortized O(1) each. It is constructed by the
type name applied, empty or pre-sized:

    var acc is buffer of int = buffer()          // empty
    var acc is buffer of int = buffer(64)        // reserve capacity 64 up front

### The API: mutation in place

Because a buffer is mutable, its operations change it in place rather than
returning a fresh value, and its verbs are spelled differently from the list's
so the difference in meaning is visible:

- **`length(b)`** is the current length (the same bare-name builtin as list and
  str).
- **`b[i]`** reads the element at 1-based `i`, fallible on an out-of-range index
  like list indexing (fallible.md).
- **`b[i] = x`** writes it **in place**, which a list cannot do (a list uses
  `set`, which returns a new list); the in-place index assignment is the visible
  mark of mutability.
- **`push(b, x)`** appends `x` in place, amortized O(1). It is `push`, not the
  list's `append`, precisely because `append` returns a new value and `push`
  mutates: the different word signals the different semantics, so a reader of
  `push` knows the buffer changed.
- **`pop(b)`** removes and returns the last element, fallible when the buffer is
  empty.
- **`clear(b)`** resets the length to zero, keeping the capacity for reuse.
- **`reserve(b, n)`** grows the capacity to at least `n` ahead of a known run of
  appends, and **`capacity(b)`** reads it. These are the backlog's "max / grow",
  a Mechanics-tier optimization; the common code never touches them.

The mutators may also be written UFCS (`b.push(x)`, `b.pop()`), `push(b, x)`
being the plain form (verbs.md decision 4, records.md).

### Reference semantics, and no `shared` needed

A buffer has identity, so assigning or passing one **aliases** it: `var b2 = b1`
makes `b2` name the same buffer, and a `push` through either is seen through
both. This is what a builder must do, and it means a buffer passed to a func is
appended in place with no `shared` marker, because the buffer is already a
reference (shared-params.md's `shared` exists to give *values* by-reference
passing; a buffer is by-reference by nature). A helper that fills a buffer is
the ordinary `fill(b, source)` with a plain `buffer of T` parameter.

### It stays actor-local: freeze to escape

A buffer never crosses the actor boundary and never persists on its own. It
cannot be a verb argument or return (values cross a send, a mutable buffer does
not), and a v1 buffer is func- and turn-local arena scratch, not a storable
field type. To let what you built **escape**, freeze it into a value:

- **`tolist(b)`** produces a `list of T`, the copy-on-write value, storable,
  sendable, shareable.
- **`tostr(b)`** produces an **owned `str`** from a `buffer of char` (or a buffer
  of appended string fragments): the owned, NUL-terminated buffer string-repr.md
  describes, born capable. Logically a snapshot, but when the buffer is consumed
  by the freeze its backing can become the owned string directly, a zero-copy
  move (the buffer already reserved the trailing NUL).

Freeze is the one gate between the mutable builder and the immutable world, the
`persistent!` boundary, and it is why a buffer can be freely mutated without any
of the sharing hazards a value type is protected from.

## Building a string

The most approachable use is a StringBuilder, assembling a string in a loop
without the O(N squared) of repeated `+` concatenation:

    func roster(names is list of str) returns str
        var b is buffer of char = buffer()
        for n in names do
            push(b, n)                 // append a fragment's bytes
            push(b, ", ")
        endfor
        return tostr(b)                // one owned str, built once
    endfunc

String interpolation and `+` already build an owned string under the hood
(string-repr.md's terminating builder); `buffer of char` exposes that builder to
the author for the incremental case a single `+` chain cannot express. The exact
byte-versus-code-point grain of appending to a `buffer of char` follows
text-encoding.md (R7) once that firms up; the buffer design here is independent
of that choice.

## Lifetime, and what waits on the memory work

A v1 buffer lives in the turn's arena and is reclaimed with it, like any turn
scratch. It is **func- and turn-local**: build, freeze, store the value. A
**persistent buffer**, one held in a field that grows across turns (an
accumulating log, a rolling history), needs a heap that outlives the arena and
so waits on the memory-management pass (backlog.md); this note does not decide
it. That is the buffer's one real dependency on the allocator work the backlog
flagged, and it is confined to the persistent case; the local builder needs
nothing beyond the arena that already exists.

## What is deferred

- **Persistent (field) buffers**, per the memory work above.
- **Arbitrary insert / remove** (`insert(b, i, x)`, `remove(b, i)`), O(N)
  shifts; v1 is the end-oriented `push` / `pop` that a builder needs. Additive
  when a use appears.
- **A guaranteed zero-copy `tostr` move**; v1 specifies freeze as a snapshot and
  leaves the move as an implementation optimization.
- **`buffer of T` of oversized elements** (a buffer of records or floats); v1 is
  word-sized elements, matching the list's element model.

## Survey

- **Clojure persistent vector + transient (`conj!` / `persistent!`)**: the exact
  model, an immutable value with a mutable local builder frozen back to a value.
  Excelsior's list / `buffer` / `tolist` is this split.
- **Rust `Vec<T>` and `String`**: growable owned buffers with `push`, capacity /
  `reserve`, and `into_boxed_slice` / freeze; `String` is `Vec<u8>` with a UTF-8
  invariant, exactly `buffer of char` to owned `str`.
- **C++ `std::vector` / `std::string` / `ostringstream`**: growable with
  `push_back`, `reserve`, `capacity`; the string stream is the StringBuilder.
- **Go slices with `append` and a backing array**: amortized growth; Excelsior
  separates the mutable buffer from the value list rather than overloading one
  slice type with both roles.
- **Java `ArrayList` / `StringBuilder`**: the mainstream mutable-builder pair,
  `StringBuilder.toString()` the freeze.
- **Zig `ArrayList`**: explicit capacity and `toOwnedSlice`, the freeze-to-value
  boundary named.

Excelsior's stance: Clojure's persistent-value / transient-builder split, spelled
`list of T` and `buffer of T` with `tolist` / `tostr` as the freeze, the buffer
kept actor-local and non-sendable so its mutability is safe, and `push` distinct
from `append` so in-place mutation reads differently from a value update.

## Decisions (confirmed)

The seven decisions are confirmed, and the element syntax is `buffer of T` per
type-parameters.md (not `buffer<T>`). The implementation (the growable backing
with doubling capacity, in-place `push`/`pop`/index-write, aliasing reference
semantics, and the freeze-to-value gate) follows; only the persistent-field case
waits on the memory-management work.

**D1. `buffer of T` is a mutable, growable, actor-local scratch container**, a
third kind beside values and actors: it has identity and in-place mutation
(which values forbid), never crosses a send and never persists on its own (which
the actor boundary forbids). Amortized O(1) append is the reason it must be
mutable, and locality is what keeps that safe.

**D2. It is constructed by the type name applied**, `buffer()` empty or
`buffer(cap)` pre-sized (context supplies `T`), with a length and a
doubling capacity.

**D3. Operations mutate in place, spelled to show it:** `b[i] = x` (in-place,
where a list uses `set`), `push` (in-place append, where a list uses the
value-returning `append`), `pop` (fallible when empty), `clear`, and the
Mechanics-tier `reserve` / `capacity` (the backlog's max / grow). `length` and `b[i]`
read as on a list, `b[i]` fallible.

**D4. A buffer has reference semantics: assignment and passing alias it**, so a
buffer passed to a func is filled in place with no `shared` marker (it is a
reference by nature, unlike a value that needs `shared`).

**D5. A buffer escapes only by freezing to a value.** `tolist(b)` yields a
copy-on-write `list of T`; `tostr(b)` yields an owned, NUL-terminated `str`
(string-repr.md), possibly a zero-copy move of the consumed backing. Freeze is
the single gate between the mutable builder and the immutable, sendable world
(Clojure's `persistent!`).

**D6. `buffer of char` is the StringBuilder**, assembling a string in a loop
without O(N squared) concatenation; it exposes the owned-string builder the
interpolation and `+` paths already use internally, with the byte/code-point
grain following text-encoding.md.

**D7. Persistent (field) buffers, arbitrary insert/remove, a guaranteed move
freeze, and oversized elements are deferred.** A v1 buffer is turn-local
arena scratch with end-oriented `push`/`pop`; only the persistent case depends
on the memory-management work, and the local builder needs nothing beyond the
existing arena.
