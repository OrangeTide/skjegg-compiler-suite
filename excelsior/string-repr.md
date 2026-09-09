# Length and null: a string that demotes to char* for free

Status: decided (2026-07), a cross-cutting design note for Excelsior and
Compact Pascal. All seven decisions confirmed. Per D7 the near-term
footprint is deliberately small: the **representation invariant (D2) is
implemented** in the Excelsior runtime (owned buffers now carry a
trailing NUL; views are the only length-only values), and the rest, the
type-level owned/view distinction and any `char*` interop surface, stays
paper until a foreign boundary needs it. Compact Pascal's part (D6) waits
on Phase 6b. See the implementation notes at the end.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

## The idea

A length-terminated string and a null-terminated string are not rivals.
An owned, immutable buffer can be **both** at once: store the length, and
also write a trailing `\0`. It then satisfies the length interface (O(1)
length, embedded-NUL-safe, sliceable) and the C interface (a `char*` that
`strlen` and every C API accept) with no conversion. The extra byte is
negligible, and for an immutable string the property is a static fact
that never decays.

Immutable string literals are the free case: the compiler emits
`[len][bytes][\0]` in the data segment once, at build time. Every literal
is born fully capable. The only values that cannot be both are **views**
(a slice into the middle of a buffer), whose trailing byte belongs to the
parent, not to a terminator.

So the type system's job is small and precise: track whether a string is
an **owned** (terminated) buffer or a **view** (length-only), carry that
through interfaces, and force a copy at exactly one place, when a view
must become a `char*`. This note is the groundwork for that.

## What exists today

**Excelsior** already sits on this spectrum without naming it. A string
is `{ int len, const char *data }`. Literals get the trailing NUL
(`intern_strlit` allocates `len+1` and writes `\0`), so they are already
len-plus-null. But `__exc_str_concat` allocates exactly `len` (no NUL)
and `__exc_str_slice` aliases the parent (`data = s->data + lo - 1`), so
concat results and slices are length-only. Output goes through
`write(fd, data, len)`, length-based, so **nothing exploits the NUL that
literals already carry.** The affordance is latent, and the day a host
power takes a `char*`, passing a slice would read past its end to the
parent's NUL: a silent over-read this note's typing prevents.

**Compact Pascal** uses Turbo Pascal short strings: `[length byte][<=255
bytes]`, on the stack or in a record, explicitly with **no null
terminator** (ref.md, String Representation). That layout is fixed by TP
compatibility and should not change. But a richer heap string (pointer +
length, no 255 cap) is deferred to Phase 6b, and *that* type is the
natural place to adopt the dual representation for `PChar` interop.

Both projects therefore have the same latent choice, at different
maturities: Excelsior has the value and needs the typing; CP has the
typing discipline (single-pass, declare-before-use) and will need the
value in Phase 6b.

## Survey

Prior art clusters around "how does the type know a string is
terminated":

- **Zig sentinel-terminated slices.** `[:0]const u8` is a slice whose
  type says it is NUL-terminated; `[]const u8` is one that does not. A
  `[:0]u8` coerces to `[]u8` for free (drop the guarantee), and producing
  a `[:0]u8` from a `[]u8` requires a terminated allocation. This is
  exactly the proposal, already realized in a type system: the
  termination is a property of the type, demotion is free coercion, and
  promotion costs an allocation. The strongest precedent.
- **Delphi/FPC AnsiString.** A heap string that stores its length *and*
  writes a trailing NUL, so `PChar(s)` is an O(1) cast, not a copy. The
  representation half of the idea, shipped for decades. What it lacks is
  the type-level owned-versus-view distinction, so a substring must be
  materialized before `PChar` is safe, and the language leaves that to
  the programmer.
- **Rust `CString` / `CStr`.** The NUL guarantee is its own type:
  `CString` owns a terminated buffer, `CStr` borrows one, and conversion
  is explicit. `CString::new` *fails on an embedded NUL*, which is the
  principled answer to the embedded-NUL question (a value that must
  become a C string may not contain an interior terminator).
- **Go strings (the cautionary case).** A Go string is immutable
  `{ptr, len}` with **no** reserved NUL, so `C.CString(s)` copies and the
  caller must free it. Every C boundary pays a copy, precisely because
  the representation did not spend the one byte up front. This is the
  cost of *not* doing the trick.

The consensus of the useful precedents (Zig, Delphi, Rust) is: reserve
the terminator on owned buffers, and let the type carry the guarantee so
the boundary cast is free. Go shows the alternative's recurring cost. The
one open axis they disagree on is embedded NUL, addressed in D5.

## Design

**One capability, two levels, carried in the type.** A string's static
type records whether it is:

- **owned** — a buffer this value's producer allocated and terminated, so
  `data[len] == '\0'` holds. Literals, concatenations, and any builder
  output are owned. Owned strings demote for free to a length-string
  (ignore the NUL) *and* to a C `char*` (pass `data`).
- **view** — a length-only window into someone else's buffer (a slice,
  a substring). Length operations work; there is no terminator. A view
  demoted to `char*` triggers a compiler-inserted copy that allocates and
  terminates (materialization), at exactly that point and diagnosably.

The lattice is small on purpose: owned is the top (satisfies both
interfaces), view is the one restricted level. Incoming C strings
(`char*` from a host, length-unknown) are not a third internal level;
the host boundary converts them to owned or view explicitly with a scan,
so the language's interior has only owned and view.

**Owned buffers always terminate.** Literals already do. The change is to
make `__exc_str_concat` (and any future builder) allocate `len + 1` and
write the NUL. After that, the *only* length-only values are views, so
the type distinction collapses to the familiar and cheap **owned vs
borrowed** shape: an owned string is a `str`, a view is a borrow of one,
and demoting a view to `char*` is "materialize the borrow," the single
forced copy.

**The capability is static; no runtime tag.** Because strings are
immutable (Excelsior CoW; CP short strings are value-copied), the
owned/view property is fixed at the point a value is produced and travels
in its type, not in a runtime bit. An interface (a verb, func, method, or
host power) declares the capability it requires and returns, so an owned
literal flows through any of them, and a `char*`-taking boundary is just
a parameter typed "owned" (or "C string"); a view reaching it is the
materialize-and-copy site.

**Embedded NUL (D5).** The length view is always authoritative, so a
length-string may hold an interior `\0`. Demoting such a value to `char*`
truncates the C view at the first NUL. Rust forbids this at
`CString::new`; for v1 the recommendation is lighter: document the
C-side truncation as the standard caveat and keep the length view
correct, rather than add a second "no-interior-NUL" capability bit now.
The bit is the recorded upgrade path if a real interop surface needs the
guarantee.

### The unifying claim

An owned, immutable, length-terminated buffer can carry a trailing NUL at
one byte's cost, and a type-level owned-versus-view distinction lets it
demote to a C `char*` for free while forcing a copy exactly when a view
crosses to C. Literals populate the capable top of the lattice for free.
That is the whole groundwork: no runtime tag, no surface syntax, no host
power required yet.

## Compact Pascal (Phase 6b)

Short strings stay exactly as they are; TP compatibility fixes their
layout, and they are value types, not the `{ptr, len}` model this note is
about. The recommendation is scoped to the **Phase 6b heap string**: give
it the owned-equals-length-plus-null representation, so `PChar(s)` on a
heap string is an O(1) cast (as Delphi's AnsiString already is), and
represent a substring of a heap string as a view that must be
materialized before it becomes a `PChar`.

This fits Compact Pascal's identity rather than fighting it. The
owned/view capability is a property of the type, known at the
declaration, so it is single-pass verifiable, the through-line the CP
spec review named as the language's discipline. Kept to two levels
(owned/view), it adds no inference and no second pass. It is worth noting
that the two projects have now converged twice, first on
compiler-generated interface files (the CP methods/interfaces review),
now on dual-terminated owned strings; convergence from two directions is
decent evidence the shape is right.

The one place the projects legitimately diverge is text encoding, not
representation: Compact Pascal chose byte semantics documented as such
(char is a byte, Unicode is a library concern), while Excelsior's R7 pass
proposes code-point character operations (text-encoding.md). That axis is
orthogonal to this one. A string can be code-point-aware or byte-oriented
independently of whether it is owned-and-terminated; this note is about
termination and ABI, R7 is about interpretation.

## Staging

Groundwork, in order of independence:

1. **Representation invariant (Excelsior).** Make `__exc_str_concat` and
   future builders allocate `len + 1` and terminate, so every owned
   string carries the NUL and views are the only length-only values. One
   byte per concat; no interface change. This is the only code change the
   note asks for now, and it is optional until interop lands.
2. **The type-level owned/view distinction.** Add the capability to the
   `str` type and thread it through the boundaries (producers mark owned
   vs view; a `char*`-typed boundary demotes owned freely and
   materializes a view). Paper until an interop surface exists; specified
   now so it is not retrofitted under pressure.
3. **Compact Pascal Phase 6b.** When the heap string is designed, adopt
   the owned=length+null representation and the owned/view distinction for
   `PChar` interop.

## Open decisions

**D1. Two levels, owned and view (recommend yes).** Model the capability
as owned (terminated) versus view (length-only), not a three-way lattice
with a separate nul-only level. Incoming C strings are converted at the
host boundary, so the interior stays two-level.

**D2. Owned buffers always terminate (recommend yes).** Literals already
do; make concat and builders allocate `len + 1` and write the NUL, so
views are the sole length-only values and the distinction reduces to
owned vs borrowed.

**D3. Free demotion, one forced copy (recommend yes).** An owned string
demotes to a length-string or a `char*` for free; a view demoted to
`char*` triggers a compiler-inserted, diagnosable materialization at that
site.

**D4. Static capability, no runtime tag (recommend yes).** Track the
property in the type; it is sound because strings are immutable, so no
value's capability changes after it is produced.

**D5. Document embedded-NUL truncation, defer the second bit (recommend
yes).** Keep the length view authoritative; a `char*` demotion truncates
at an interior NUL, documented as the standard C caveat. Add a
"no-interior-NUL" capability bit only if a real interop surface needs the
guarantee (the Rust `CString` model is the recorded path).

**D6. Compact Pascal: Phase 6b heap string, short strings unchanged
(recommend yes).** Short strings keep the TP layout; the heap string
adopts owned=length+null and the owned/view distinction for `PChar`
interop, kept to two levels so it stays single-pass verifiable.

**D7. Groundwork scope (recommend yes).** Land at most the representation
invariant (D2) now; the type-level distinction and any interop surface
follow when a foreign boundary actually needs a `char*`. The note exists
so that boundary is free when it arrives, not to build it early.

## Implementation notes

Per D7, one change landed, in the Excelsior runtime (now
`runtime/libexc.c`; `exc_host.c` at the time):
every **owned** buffer now carries a trailing NUL, so the owned invariant
holds and views are the sole length-only values.

- `make_str` (the builder behind `__exc_str_from_int/float/dec/bool`, the
  interpolation-hole conversions) allocates `n + 1` and writes `buf[n] =
  '\0'`.
- `__exc_str_concat` allocates `len + 1` and terminates.
- Literals already terminated (`intern_strlit` in lower.c allocates
  `len + 1` and writes the NUL).
- Slices (`__exc_str_slice`, `__exc_str_at`) are unchanged: they alias the
  parent buffer and are the intended length-only views.

The arena 4-byte-aligns every allocation, so the extra byte is
alignment-transparent (a length-`n` buffer already rounded up to the next
word). The change is behavior-neutral, since every string operation reads
`len`, not the terminator; the full suite (check-exc 45/45, check
262/262) confirms no program changed. Nothing observes the NUL yet, by
design: the type-level owned/view distinction (D3) and a `char*`-taking
boundary are deferred to when interop actually needs them. This commit
only makes the invariant true, so that boundary is free when it arrives.
