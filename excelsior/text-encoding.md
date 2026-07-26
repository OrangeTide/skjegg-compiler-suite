# Text is bytes, and the audience writes text

Status: decided (2026-07), implemented. The pass on approachability.md's
R7 (text is bytes, and the audience writes text). The decisions at the end are
confirmed, with D5 adjusted to add a low-level `bytes` accessor. Implemented:
the vendored `runtime/utf8.c` decoder, code-point `len`/`s[i]`/`s[lo to hi]`/`for
c in s` and the code-point index bound, and the `bytes` accessor for str and
list; see `tests/exs_utf8_r7.exs`.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

## The problem

An Excelsior string is a `{ len, data }` descriptor where `length` is a
**byte** count (string-plan.md). Every character operation is therefore
byte-level:

- `length(s)` returns the byte count.
- `s[i]` and `s[lo to hi]` index and slice by byte position.
- `for c in s` walks byte positions, yielding a one-byte string each
  step.
- `<` `<=` `>` `>=` compare byte by byte; `==` is byte-exact.

The audience is content creators, who will write accented and non-Latin
text on the first day. In UTF-8 those characters are two, three, or four
bytes, so byte indexing splits a code point: `length("café")` is 5, `s[4]`
is the first half of `é`, and `for c in "café"` yields five fragments,
two of them invalid on their own. No document states any policy, not even
"v1 is bytes, non-ASCII indexing is unsupported."

There is a smaller related point. The language folds identifier case (a
declared principle), but string `==` is byte-exact and case-sensitive.
That is a defensible split, but it is undocumented and worth one
sentence, because case-insensitivity is advertised.

The whole approachability objective is aimed at the person this trap hits
first. A design pass that shrugs and documents "it's bytes" fails that
person on their first line of dialog.

## What exists today

- **Storage is UTF-8-clean already.** A literal's bytes are copied
  verbatim into an `IR_I8` blob (`intern_strlit`, lower.c); nothing
  decodes or re-encodes them. Whatever UTF-8 the source carries survives
  into the descriptor.
- **The pass-through operations are encoding-agnostic and correct.**
  Concatenation (`__exc_str_concat`), interpolation holes (each hole
  stringified and concatenated), and output (`tell`, a byte `write`)
  move bytes without interpreting them. A UTF-8 string built from holes
  and concatenation and printed comes out byte-for-byte intact. These
  need no change.
- **Only the character operations interpret positions**, and they all use
  bytes: `length` reads word 0 (the byte count), `__exc_str_at`/
  `__exc_str_slice` index by byte, `for c in s` loops `1..bytelen`, and
  `__exc_str_cmp` walks bytes.
- **Ordering is already code-point-correct by accident.** UTF-8 is
  designed so that byte-wise lexicographic comparison yields the same
  order as code-point comparison. So `__exc_str_cmp` sorts by code point
  today, with no change needed; only `length`, indexing, slicing, and
  iteration are wrong for multibyte text.

So the surface area of the problem is four operations, and the storage
and pass-through halves are already right.

## Survey

Languages split four ways on "what is a character":

- **Bytes** (C, Lua 5.x). A string is a byte array; the encoding is the
  programmer's problem. Honest and simple, but it pushes UTF-8 decoding
  onto every user, which is the opposite of an approachable default.
- **UTF-8 storage, bytes for random access, code points for iteration**
  (Go). Go stores UTF-8, `length(s)` and `s[i]` are bytes, but `for _, r :=
  range s` yields runes (code points). The direct precedent for "keep
  UTF-8 bytes, decode on iteration." Go accepts a split (index is bytes,
  range is runes) that a systems audience tolerates.
- **UTF-8 storage, code points as the unit, byte indexing forbidden**
  (Rust). `str` is UTF-8; you cannot write `s[i]` for a single char, you
  use `.chars()` (code points) or `.bytes()` explicitly. The encoding is
  visible but the character unit is the code point.
- **Code points as the abstraction** (Python 3: `str` indexes and lengths
  in code points, hiding the encoding via a fixed-width internal
  representation) or **grapheme clusters** (Swift: `Character` is a
  user-perceived grapheme, the most correct and the heaviest, needing
  Unicode break tables).

The gradient is bytes to code points to graphemes: cheaper and more
leaky, versus costlier and more correct. For a content creator writing
display text, bytes are wrong on day one and graphemes need Unicode
tables a v1 m68k VM should not carry. Code points are the sweet spot the
two UTF-8 precedents (Go, Rust) both land near: correct for the common
NFC text an author types, and reachable with a small decoder and no
tables. The one refinement over Go: do not keep a bytes-versus-runes
split across `s[i]` and `for c in s`, because a beginner cannot hold "one
of these counts bytes and the other counts characters" in their head.
Make every character operation count the same unit.

## Design

**Strings are UTF-8; the character operations count code points.**
Storage stays the `{ byte-len, data }` UTF-8 descriptor. The four
character operations decode:

- `length(s)` returns the **code-point count**.
- `s[i]` is the one-code-point string at code-point position `i`
  (1-based), and stays a fallible producer (out of range traps or is
  caught by `else`, unchanged).
- `s[lo to hi]` slices by code-point positions, clamped as today; the
  result still aliases the parent buffer (a code-point slice is a byte
  range, so no copy).
- `for c in s` yields each code point as a one-code-point string (one to
  four bytes), so `c == "é"` works.

**A low-level `bytes` accessor sits beside `length`.** `length(s)` is the
code-point count an author reasons about; `bytes(s)` is the raw UTF-8 byte size
(`byte-len`), for a runtime author who needs to size a transfer or a copy without
regard to code points. The two split the old byte-versus-code-point tension by
role rather than by operation: `length` counts what the author sees, `bytes`
counts storage. The pair generalizes to any array, not just strings: `length(xs)`
is the number of elements and `bytes(xs)` is the raw byte size of its element
storage (`element-size` times `length`), so "how many units" and "how many raw
bytes" are one uniform question across strings and arrays. `bytes` keeps a
deliberately technical name (over `count`), because it is a Mechanics-tier tool
for runtime authors, not everyday author surface. It returns a size, not a byte
array; a distinct raw-byte view or type stays deferred (D5).

**The pass-through operations stay byte-level** (concat, holes, output,
literals): they are encoding-agnostic and already correct.

**Ordering stays byte-wise**, which equals code-point order for UTF-8, so
`<`/`<=`/`>`/`>=` are unchanged and already sort by code point.

**Equality stays code-point-exact**, which is the same as byte-exact once
both sides are well-formed UTF-8. No normalization: `é` written as one
code point and `e` plus a combining accent are different strings. No case
folding: string comparison is case-sensitive; case folding is an
identifier-resolution rule, not a text rule. This is the one documented
sentence R7 asks for.

**Invalid UTF-8 is lenient.** A byte that is not a valid UTF-8 lead, or a
truncated sequence, counts as a single one-byte unit rather than trapping
or crashing. So ASCII and well-formed UTF-8 behave correctly, protocol or
binary junk stays addressable, and no input can fault the decoder. (An
alternative, trapping on malformed input, is recorded under D4.)

**Grapheme clusters are out of scope.** A code point is not always a
user-perceived character (combining marks, emoji ZWJ sequences), so
`length` over decomposed or compound emoji text can exceed the visual count.
This is the documented v1 limitation; grapheme support waits for Unicode
break tables and a reason to carry them.

### Cost

Code-point `length`, indexing, and iteration become O(n) byte scans instead
of O(1), and slicing does one scan to map code-point bounds to byte
offsets. Author text (dialog lines, item names) is short, so this is not
a concern; if a hot path over long strings ever appears, a cached
code-point length or a byte-access escape hatch (D5) is the answer, not
byte-level defaults for everyone. The runtime gains a small UTF-8
decoder (a few lines: read a lead byte, consume 0 to 3 continuation
bytes), shared by the four helpers.

### The principle

The character operations count what the author sees as characters, as
closely as v1 can afford (code points, not bytes, not yet graphemes).
The encoding is UTF-8 and stays out of the author's way: storage,
concatenation, interpolation, and output never decode, so text flows
through whole, and only the four operations that name a position pay for
a decode.

## Staging

1. **Document the policy** in string-plan.md and CLAUDE.md: UTF-8
   storage, code-point character operations, byte-wise ordering,
   code-point-exact equality, no normalization, no case folding,
   graphemes deferred.
2. **A UTF-8 decoder plus the four helpers.** Add the decoder to the
   runtime and route `__exc_str_len` (the `length` path), `__exc_str_at`,
   `__exc_str_slice`, and the `for c in s` step through it. The lowering
   shape is unchanged; only the helpers reinterpret positions. There is
   no positive test artifact for the current byte behavior to protect,
   and the ASCII path is unaffected (one code point per byte), so ASCII
   tests stay green; new tests cover accented text. Do not write the decoder:
   vendor the battle-tested `~/Vibe/lumi/src/libutf8/utf8.{c,h}` (MIT-0 or
   public domain, freestanding, only `<stddef.h>`/`<stdint.h>`), whose
   `utf8_decode` already implements D4 exactly (a bad or truncated byte yields
   `UTF8_RUNE_ERROR` and consumes one byte, never faulting) and rejects overlong
   forms, surrogates, and out-of-range values. Take only `utf8.c`/`utf8.h`; the
   sibling `rune_width.c`/`width_tables.c` are the grapheme/width tables this
   pass deliberately defers and must not be pulled in.

## Decisions (confirmed)

The five decisions are confirmed, with D5 adjusted to add a low-level `bytes`
size accessor. The implementation (a small UTF-8 decoder shared by the four
character helpers, `bytes` returning the stored byte length, and the documented
policy) follows; grapheme support, a raw-byte view/type, and byte indexing stay
deferred.

**D1. UTF-8 storage (confirmed).** Keep the `{ byte-len, data }`
UTF-8 descriptor; do not adopt a wide fixed-width internal representation.
Storage and pass-through are already correct and cost nothing; only the
position-naming operations decode.

**D2. Code points as the character unit (confirmed).** `length`, `s[i]`,
`s[lo to hi]`, and `for c in s` all count code points, consistently.
Alternatives: bytes (wrong for the audience on day one) or graphemes
(needs Unicode tables, deferred). Go's index-bytes/iterate-runes split is
rejected as one distinction too many for a beginner.

**D3. Byte-wise ordering, code-point-exact equality, no normalization or
case folding (confirmed).** Ordering is already code-point-correct
(a UTF-8 property) and unchanged; equality stays exact; string comparison
is case-sensitive, and the identifier case-fold rule does not extend to
text. Document the one sentence R7 asks for.

**D4. Lenient handling of invalid UTF-8 (confirmed).** A malformed
byte counts as one unit; the decoder never faults. Alternative: trap on
malformed input (an INDEX_RANGE-style fault). Lenient is chosen so
protocol and binary bytes stay usable and no input can crash a decode.

**D5. A `bytes` size accessor is added; byte indexing and a raw-byte type stay
deferred (adjusted).** `bytes(x)` returns the raw byte size, a string's stored
UTF-8 `byte-len` or an array's element-storage size (`element-size` times
`length`), for a runtime author sizing a transfer or copy without regard to code
points. It is uniform over strings and arrays beside `length` (units), a
deliberately technical Mechanics-tier name over `count`. It returns a size, not a
byte array: a distinct raw-byte view or type, and any byte *indexing* escape
hatch, stay deferred until a demonstrated need. A future capacity/`max`-style
accessor for growable arrays is noted but undefined here (it overlaps buffer.md's
`capacity`).
