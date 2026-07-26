# Blobs: binary data for v1, a mutable refcounted byte view

Status: decided (2026-07), not yet implemented. The binary-data pass from the
backlog (backlog.md), raised while implementing R7 (text-encoding.md). Tier: Mechanics; a game author packs a
protocol message or reads an image header. A host-backed blob (viewing real host
memory) is additionally a host power (host-abi.md); a script-allocated blob is
ordinary. Builds on the reference-counted heap (memory.md), the code-point `str`
and its `bytes()` accessor (text-encoding.md), the meta layer (meta.md: a macro
reads its arguments as forms), the signed-only integer model, and the fallible
index (fallible.md).

A game server does binary work its C code will not always pre-bake: pack a TELNET
negotiation or an ANSI/VT320 escape sequence, read a PNG or JPEG header to bubble
up the image size (to scale a fog-of-war layer over a map), assemble a small
protocol frame. These are game features that live better in the scripting layer
than in a server rebuild. A **`blob`** is the v1 answer: a **mutable, bounded,
reference-counted byte buffer** with typed offset read and write. It is
memory-safe by construction (refcounted backing, bounded access) yet mutable and
aliasing, so it does the systems work the immutable value world cannot, without
the immutable value world's copies.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

## The v1 job: manipulate binary, both directions

Blob exists to read and to build binary:

- **Build** (write): allocate a blob, write typed fields at offsets, hand the
  bytes to the host to send. A TELNET frame or an ANSI escape is a short,
  known-layout byte sequence assembled field by field.
- **Parse** (read): receive a blob (a header the host handed in, or one built
  from a `str`'s bytes), decode typed fields at offsets. A PNG's width and height
  are two big-endian 32-bit integers at a known offset.

So the blob is read/write from the start, not a read-only decoder.

## A view: refcounted, bounded, and therefore safe

A blob is a **view**, an offset and a length, over a byte buffer. The buffer is
either **script-allocated** (an ordinary allocation) or **host-granted** (a real
file or texture the host owns, a host power). Two guarantees make the view safe
without making it immutable:

- **The backing is reference-counted (memory.md), so a blob keeps it alive.** A
  buffer cannot be freed while any blob views it, so a blob can never dangle into
  freed memory: no use-after-free, which is the memory-safety a sandbox requires.
- **Access is bounded to the view's length.** `blob[i]` and every decode is
  fallible on the view's range, so a script can never read outside its window.
  This is the other half of sandbox safety: a blob is not an arbitrary read of
  the shared server's memory, and it cannot be forged.

Within those bounds the blob is deliberately **loose**: its contents are
**mutable** (you build them up, or a live host buffer changes under you), and a
**slice is a range-limit, not a copy** (`blob[lo to hi]` narrows the window over
the same backing, so a write shows through the blob and every slice of it; a
sub-view refcounts the backing directly). This aliasing is the point, a decoder
walks a frame by re-slicing it. It is looser than the immutable value world but
it is not C's hole: refcounting removes use-after-free and bounding removes the
arbitrary read, so what remains is *semantic* looseness (aliasing, a buffer that
changes), defined behavior a systems-minded author reasons about, not memory
corruption.

The one real cost is **pinning**: a script holding a blob keeps its backing
alive, so a host buffer stays resident until the script releases it, with no easy
forced reclaim short of ending the actor. That is a resource concern, bounded by
the memory quota (memory.md), not a safety hole.

## Typed access is a decode macro over literal keywords

Bytes carry no type, so a wider value comes out (and goes in) by stating the
three things the bytes cannot: **width** (8, 16, 32 bits), **signedness** (signed
or unsigned), and **endianness** (big or little, moot at 8 bits). The surface is
a single readable form:

    frame.read_int(0, 16, unsigned, little)      // a u16 at offset 0, LE
    png.read_int(16, 32, unsigned, big)          // PNG width: BE u32 at 16
    frame.write_int(2, 32, signed, big, value)   // pack an s32 at offset 2

`read_int` is a **macro**, which is what lets `unsigned`, `little`, and `16` be
bare keywords with no global symbol behind them: a macro receives its arguments
as **forms**, not evaluated values (meta.md), so it reads `unsigned`/`little` as
literal keyword forms and expands to the one monomorphic decode. So the
parameterized form is also the constant-folded form (there is no runtime dispatch
on width or endian; the macro emits the specific load), and no `Endian`/`Sign`
enum needs to exist. Width, sign, and endian must be literal keywords at the call
(always true for a real format, like patterns.md's literal patterns). Terser
named aliases (`read_be32u`) are trivial macro wrappers if wanted.

This keeps the **whole matrix contained in the blob type**: the language keeps
one integer, signed and native, and never grows an endianness, width, or
unsigned-arithmetic concept, because every one of those questions is answered at
the blob's edge. It is R7's containment (UTF-8 lives in the string helpers)
applied to binary layout.

## Signed and unsigned: reinterpret now, a real unsigned type later

Excelsior's `int` is signed and 32-bit today, so v1 **reinterprets the bytes'
bit pattern into a signed `int`**: a `u8`/`u16`/`s8`/`s16` widens cleanly, and a
`u32` is the raw bits as a signed `int` (so `0xFFFFFFFF` reads as `-1`), the cheap
C-style slam a systems author expects, no fault. The `signed`/`unsigned` keyword
therefore selects sign- versus zero-extension for the narrow widths and is a
no-op at width 32 (the bits are the value either way).

A proper **unsigned integer type** is the better long-term answer, and it is
deferred: it would let a decode return an unsigned value and catch an
out-of-range *store* at compile time (refuse it, or require an explicit check or
coercion) rather than reinterpret. That is a whole scalar family and does not
block blob; until it lands the reinterpret keeps a content author's ordinary math
plain signed `int`, with unsignedness contained at the blob boundary.

## Bridges: str and the byte builder

A `str` and a blob meet at their edges: a `str` exposes its UTF-8 bytes as a blob
(reading them is safe; writing through a str-backed blob is undefined, since a
`str` is immutable), and a blob decodes to a `str` as UTF-8 (R7's lenient decode).
The growable, arena-managed `buffer of byte` (buffer.md) stays the tool for
building a byte sequence of unknown length; a blob is the fixed-window typed
accessor, and a blob can be taken over a buffer's bytes. Growth and the typed
window are two jobs, one type each.

## What is deferred

- **An unsigned integer type** with compile-time out-of-range-store checking (the
  real fix for the reinterpret above); and a **64-bit `int`**, which adds 64-bit
  rows to the decode matrix.
- **The named-alias vocabulary's final form**, **blob literals** (a hex or byte
  sequence), **bit-level access** (sub-byte fields, Erlang's bit syntax), and
  **alignment**.
- **The host grant/revoke specifics** for a host-backed blob, with the
  host-abi.md powers work. (A sub-view refcounts the backing directly, not the
  parent blob, so every view is a flat reference to one refcounted buffer.)

## Survey

- **Erlang/OTP bit syntax** (https://www.erlang.org/doc/system/bit_syntax.html):
  a binary type carrying size, signedness, and endianness in segment specifiers,
  with pattern-matching over them, the strongest "layout lives in the binary
  type" precedent; Excelsior takes that as a decode macro, bit-level matching
  deferred.
- **Go `encoding/binary`** (https://pkg.go.dev/encoding/binary): the named-method
  matrix keyed by an endianness value (`binary.LittleEndian.Uint16`); the terse
  aliases here are that shape, over the one macro.
- **Python `struct`**: format strings encoding the matrix compactly (`">II"`),
  contained but cryptic against words-over-symbols.
- **Node.js `Buffer`**: a mutable, refcounted byte view with `readUInt16LE` and
  friends, close to this blob in role.
- **LPMud LPC**: the lineage, low-level buffer access grown to add server
  features, the motivation for a byte-manipulation type in an in-world language.

Excelsior's stance: a `blob` is a mutable, reference-counted, bounded byte view,
the v1 answer for manipulating binary data (protocols, escape codes, image
headers); it is memory-safe by construction (refcounted backing, bounded access)
while staying mutable and aliasing; typed access is a decode/encode macro over
literal width/sign/endian keywords that keeps binary layout inside the type; and
the signed/unsigned boundary is a reinterpret to signed `int` now, with a real
unsigned type deferred.

## Decisions (confirmed)

The seven decisions are confirmed, with a sub-view refcounting the backing
directly (D2). The implementation (a refcounted byte buffer with bounded,
fallible offset access, the decode/encode macro over literal width/sign/endian
keywords expanding to monomorphic loads/stores, the signed reinterpret, and the
str/`buffer of byte` bridges) follows the meta layer (the macro) and the
reference-counted heap (memory.md); a real unsigned type, a 64-bit `int`,
literals, bit-level access, and the host grant/revoke lifecycle are deferred.

**D1. A `blob` is a mutable, reference-counted, bounded byte view, the v1
binary-data type.** It reads and writes typed fields at offsets, for building
protocol messages and escape codes and parsing headers. Its backing is
script-allocated (ordinary) or host-granted (a host power, host-abi.md). Tier
Mechanics.

**D2. The blob is memory-safe by construction, and semantically loose within.**
The backing is refcounted so a blob keeps it alive (no use-after-free), and
access is bounded to the view's length (no arbitrary read, cannot be forged),
which together are the sandbox-safety guarantee. Within, contents are mutable and
a slice is an aliasing range-limit (not a copy), so a write shows through every
slice; a sub-view refcounts the backing. The one cost is pinning (a script holds
its host backing alive), a quota-bounded resource concern, not a safety hole.

**D3. Typed access is a decode/encode macro over literal keywords.**
`read_int(offset, width, sign, endian)` and `write_int(offset, width, sign,
endian, value)`; because a macro reads its arguments as forms (meta.md),
`unsigned`/`little`/`16` are bare keywords needing no global symbol, and the
macro expands to the one monomorphic, constant-folded load or store. Width, sign,
and endian are literal at the call. Terser named aliases (`read_be32u`) are macro
wrappers.

**D4. The width/sign/endian matrix is contained entirely in the blob type.** The
language keeps one signed, native integer and never grows endianness, width, or
unsigned-arithmetic concepts; every such question is answered at the blob's edge
(R7's containment for binary layout).

**D5. The signed/unsigned boundary is a reinterpret to signed `int` for v1, not a
fault.** `u8`/`u16`/`s8`/`s16` widen cleanly; a `u32` is its raw bits as a signed
`int` (`0xFFFFFFFF` is `-1`), the C-style slam a systems author expects. A proper
unsigned type (returning unsigned, refusing an out-of-range store at compile time)
is the deferred better answer; a 64-bit `int` is deferred with it.

**D6. A blob and a str convert only at their edges** (a str's UTF-8 bytes as a
blob, read-only; a blob decoded as UTF-8), and the growable `buffer of byte`
(buffer.md) stays the builder of unknown-length byte sequences while the blob is
the fixed typed window; a blob may view a buffer's bytes.

**D7. Deferred:** an unsigned type and a 64-bit `int`, the named-alias form, blob
literals, bit-level access, alignment, and the host grant/revoke lifecycle (with
host-abi.md).
