# `of`, not `<T>`: spelling a type's element with a word

Status: decided (2026-07), not yet implemented. A cross-cutting surface pass, prompted by the observation that
`list<T>` reads like C++ in a language that spells every other type structure
with a word. Tier: World (tier 1); a `list of obj` field is tutorial surface
(tiers.md). Touches the implemented `list<T>` and buffer.md's `buffer<T>`.

Excelsior spells type structure in words: `maybe T`, `set of Element`, `any of
(int, str)`, `returns T`, `x is int`, `class Chest is Container`. The one
holdover is the angle-bracket generic `list<T>` (and, following it, buffer.md's
`buffer<T>`), carried in from C++ / Java / Rust. This pass replaces it with the
word `of`, so `list<T>` becomes **`list of T`**.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

## Why `<T>` is the wrong fit

Three reasons, each on its own enough:

- **It contradicts the language's own type vocabulary.** Everything else that
  parameterizes or ascribes a type is a word: `maybe T` (an optional), `set of
  Element` (set-of.md), `any of (...)` (union-types.md), `is`, `returns`.
  `list<T>` is the only place a type reaches for a symbol, and it is the only
  one a reader cannot say aloud.
- **`<` and `>` are the comparison operators.** `list<int>` reuses the exact
  tokens `a < b` uses, which is the source of C++'s notorious parse ambiguity
  (`a < b > c` versus `Foo<Bar>`, and the `>>` that closes two nested generics
  yet also means shift). The type parser must special-case `<` in type position,
  and `list<list<int>>` needs the `>>` disambiguation C++ spent years on. A word
  makes the whole class of problem vanish: `of` is never a comparison.
- **It violates "words do structure, symbols do math" and searchability.** The
  design principle reserves symbols for arithmetic and comparison and gives
  structure to words, because a word reads aloud and can be looked up while `<T>`
  cannot (core.md, the same reasoning that removed `->`, `..`, and `?:`).

## The design: `of` is the element connector

**A container type names its element with `of`.** The container word comes
first (prefix, like `maybe T`), then `of`, then the element type:

    var contents is list of obj
    var scores   is list of int
    var acc      is buffer of char
    field tags   is list of str

This unifies with the type structure already decided: `set of Element`
(set-of.md) and `any of (int, str)` (union-types.md) already use `of`, so
`list of` and `buffer of` complete the pattern rather than adding a new one.
`of` is the one connector for "parameterized by", a single element type after it
(`list of int`) or a parenthesized alternation for a union (`any of (int,
str)`).

**Nesting is an `of` chain, with no `>>` hazard:**

    list of list of int              // a list of lists
    list of any of (int, str)        // a list whose elements are int-or-str
    buffer of str                    // a string-fragment builder

`list of list of int` reads left to right and needs none of the `>>`
disambiguation `list<list<int>>` forces. The union case parenthesizes its
alternatives (union-types.md's rule), so `list of any of (int, str)` is
unambiguous.

**Multi-parameter types use a second small word.** A future map or dictionary,
with a key type and a value type, reads `map of K to V` (`map of str to int`),
reusing `to` as the second connector the way ranges and slices already do. Maps
are not in the language yet, so this is the recorded shape, not a v1 addition;
the single-element `of` is what lands now.

Pascal is the direct precedent and the audience's: `array of T`, `set of T`, and
`file of T` all spell the element with `of`, and the builders Excelsior targets
already read it. So `list of int` is not a novelty, it is the readable form the
Algol family used before the angle bracket.

## What retires

- **The `<T>` angle-bracket form.** `<` and `>` return to being solely the
  comparison operators, and the type parser drops its `<`-in-type-position
  special case and the `>>` split.
- **The lexer answers the old form with a migration hint.** `list<int>` in
  source gets `there is no <...> for a type; write "list of int"`, the same
  migration-hint treatment as the removed `->` and `..`.
- **The migration is incremental**, the spec leading. This note and buffer.md
  adopt `of` now; the implemented `list<T>` and the many `list<...>` references
  across the notes and CLAUDE.md are rewritten as they are touched, exactly as
  the de-arrow decision propagated. A reader who sees a stale `list<int>` reads
  it as the pre-reform spelling of `list of int`.

## Survey

- **Pascal / Ada**: `array of T`, `set of T`, `file of T`; the `of` element
  connector this pass adopts, and the audience's own dialect.
- **ML / OCaml / Haskell**: `int list`, `int array` (postfix words). Words, but
  postfix, which would fight Excelsior's prefix `maybe T` and `set of E`; `of`
  keeps the container-first reading.
- **Go**: `[]T`, `map[K]V` (symbols); readable-ish but symbolic, the pole
  Excelsior's words-for-structure principle declines.
- **C++ / Java / Rust / C#**: `vector<T>`, `List<T>`, `Vec<T>`, the
  angle-bracket family this pass leaves, along with its `<`/`>` overloading and
  `>>` parse hazard.
- **Swift**: `[T]` and `Array<T>` both; the sugar and the generic side by side,
  the inconsistency a single `of` avoids.

Excelsior's stance: Pascal's `of` element connector, prefix and word-based, one
rule across `list of T`, `buffer of T`, `set of E`, and (with `to`) a future
`map of K to V`, retiring the angle bracket and its comparison-operator
overloading.

## Decisions (confirmed)

The five decisions are confirmed. `of` is the element connector, `<T>` retires.
The implementation reworks the type parser to read `list of T` / `buffer of T`,
drops the `<`-in-type special case, and adds the migration hint; the many
`list<...>` references across the notes and the implemented `list<T>` migrate
incrementally, the spec leading.

**D1. A container type names its element with `of`: `list of T`, `buffer of
T`.** Prefix and word-based, unifying with the already-decided `set of E` and
`any of (...)` and consistent with `maybe T`. `of` is the single element
connector.

**D2. `<T>` angle brackets retire.** `<` and `>` are solely the comparison
operators again; the type parser drops the `<`-in-type special case and the
`>>` split. The lexer answers a stale `list<int>` with a migration hint.

**D3. Nesting is an `of` chain (`list of list of int`), and a union element is
`list of any of (...)`.** No `>>` disambiguation; the union parenthesizes its
alternatives.

**D4. Multi-parameter types use `of ... to ...` (`map of K to V`), deferred with
maps.** The recorded shape for a future keyed collection; the single-element
`of` is what lands now.

**D5. The migration is incremental, the spec leading (like the de-arrow).** This
note and buffer.md adopt `of`; the implemented `list<T>` and the `list<...>`
references across the notes migrate as touched, a stale form reading as the
pre-reform spelling.
