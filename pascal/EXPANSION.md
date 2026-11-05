# skj-pc, the Pascal front end

`skj-pc` is a Pascal compiler over the Skjegg shared IR. The language is a Pascal
dialect in the Free Pascal lineage: it takes the Borland and FPC procedural core
and the ergonomics that come with it (managed strings, dynamic arrays,
overloading, `out` parameters, typed constant aggregates, include files,
external declarations), but it does not implement the Delphi and Object Pascal
object model. There are no classes, objects, interfaces, or RTTI.

The front end began as just enough Pascal to compile `kobold/kpcom.pas`, the
Kobold compiler (~12.8k lines). That is still the acceptance program, but the
language now stands on its own: it compiles ordinary Pascal programs, not only
`kpcom.pas`.

The language reference (what is and is not accepted, with the grammar) is
`doc/languages/pascal.md`. This document is the design and the roadmap.

## What it compiles today

Scalars and numeric types:

- `Integer`/`LongInt` (32-bit), `Int64`/`QWord` (64-bit), `Boolean`, `Char`.
- `Real`/`Double` (f64) and `Single` (f32), with `/` always yielding a real, and
  `trunc`, `round`, `int`, `sqrt`, and the numeric casts.
- Type aliases (`type X = Y`), subranges (`type X = lo..hi`), and enumerated
  types registered as ordinal types (so `High`/`Low`, `array[TColor] of T`, and
  `set of E` work). Explicit enum ordinals (`a = 3`) are not implemented.

Managed types, reference counted in the FPC ansistring style (retain and release
emitted at assignment and scope exit, copy on write, leak-clean under a live
AddressSanitizer plus LeakSanitizer gate):

- Heap strings (the 255-byte ShortString is gone; `string[N]` behaves as an
  unbounded ansistring), with concat, `copy`, `delete`, `insert`, indexing,
  in-place self-append (`s := s + e` amortizes linearly), and copy-on-write
  character mutation (`s[i] := c`).
- Dynamic arrays (`array of T`) with `SetLength`/`Length`/`High`/`Low`, managed
  element types (`array of string`, arrays of records with string fields),
  open-array parameters, and nested `array of array of T`.
- Managed members inside records and arrays, retained and released recursively
  on copy, parameter passing, and scope exit.

Declarations and routines:

- `out` parameters, routine overloading by arity and type, nested routines,
  `forward`.
- Typed constant aggregates with named fields, const arrays of records, and
  const arrays indexed by an enum.
- `Str(v, dest)` where the destination may be a variable, field, element, or the
  enclosing function's result.

Externals and host imports (the `KOBOLD`-defined build of `kpcom.pas` uses
these):

- `external cdecl` declarations, linking by the exact source spelling so a
  CamelCase host import such as `ReadFileStr` resolves.
- `{$I file}` / `{$INCLUDE}` splicing anywhere, including expression position
  (`KP_VERSION = {$I version.inc}`).
- `Write`/`WriteLn` stream selection through a leading `Output` or `ErrOutput`
  argument, `Flush` as a no-op, and `ParamStr`/`ParamCount`.

FPC-compatible evaluation details the real source depends on: short-circuit
`and`/`or` (`{$B-}`), right-to-left argument evaluation, bitwise integer `not`,
`nil` as the null pointer, and a right side evaluated before the destination
address is taken.

## Design

- **Native AOT, host targets.** One front end, one binary per target, linked
  with that target's selector, mirroring `skj-cc`. The backends are x86-64,
  AArch64, ColdFire, and MIPS (the MIPS front end, `skj-pc-mips`, feeds the
  PlayStation SDK). x86-64 and AArch64 assemble with the in-tree assemblers (no
  nasm, no Apple tools).
- **Host libc runtime.** The runtime is one C glue file over `malloc`/`stdio`,
  the role `runtime/pascal_rt.c` fills. The freestanding build (ColdFire, MIPS)
  substitutes a small bump allocator and link-complete stubs for the host-only
  pieces.
- **Managed memory.** Strings and dynamic arrays are reference counted with
  copy-on-write. Ownership is classified syntactically (a literal or a
  concatenation is owned and moved; everything else is borrowed and retained),
  the sound direction because over-retaining only leaks while under-retaining
  frees too early. The whole host suite runs under ASan plus LSan so a leak,
  double free, or use after free fails a test.
- **Target model.** LP64 on x86-64 and AArch64 (8-byte pointers and managed
  descriptors, 8-byte `Int64`, 32-bit `Integer`/`LongInt`); ILP32 with 4-byte
  slots on ColdFire and MIPS. The front end selects pointer width from a
  compile-time flag, as the C front end does. On a host build the Pascal program
  entry is renamed off `main`, and a C crt owns `main(argc, argv)`, captures the
  arguments for `ParamStr`/`ParamCount`, and calls the Pascal entry.

## Acceptance

`make check-pc-kpcom` builds `kpcom.pas` with `skj-pc` (with `KOBOLD` defined, so
its file I/O is `external cdecl` host imports), links it against the host runtime
and the host-import shims, and checks that it emits byte-identical KPM to the
reference Kobold front end over the whole behavioral corpus (all 86 modules
match). The per-target Pascal test suites (`tests/pascal_*.pas`) run on every
backend, and the x86-64 suite always runs under the ASan plus LSan leak gate.

## Roadmap

The direction is a general-purpose Pascal large enough to compile real programs,
covering more of the ISO Extended Pascal and Free Pascal procedural core as
programs need it. Candidate areas, pulled in when a real program calls for them:

- A real `file`/`Text` type and the classic file I/O verbs (today the Kobold
  build routes I/O through `external cdecl` host imports instead).
- A broader standard-routine set and wider ordinal and set coverage.
- Explicit enumerated-type ordinals and richer subrange and variant handling.

Out of scope, by design: the Delphi and Object Pascal object model (classes,
objects, interfaces, properties on classes, RTTI, exceptions as an object
hierarchy). The extensions here stay procedural and FPC-compatible, not
Object Pascal.
