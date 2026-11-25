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
  `trunc`, `round`, `int`, `sqrt`, `sin`, `cos`, `ln`, `exp`, `arctan`, `pi`, and
  the numeric casts.
- Typed pointers (`^T`), `p^` dereference (chaining with `.f` and `[i]`),
  `New`/`Dispose`, `GetMem`/`FreeMem` (an untyped block of an explicit byte
  size), and `nil`. A pointer is an unmanaged machine-word value.
- Sets (`set of T`) over the 0..255 element universe (a fixed 256-bit value)
  with union `+`, difference `-`, intersection `*`, symmetric difference `><`
  (the FPC operator), `in`, and the relations `=`/`<>`/`<=`/`>=`. A constant set
  element outside 0..255 is a compile error rather than a silent clamp.
- Type aliases (`type X = Y`), subranges (`type X = lo..hi`), and enumerated
  types registered as ordinal types (so `High`/`Low`, `array[TColor] of T`, and
  `set of E` work). Explicit enum ordinals (`Tue = 10`) are supported; an
  unnamed member continues from the previous value, and explicit values must
  ascend.
- Procedural types (`procedure(...)` and `function(...): T`, named or inline,
  with optional `cdecl`). A value is a bare code pointer taken with `@routine`,
  held in a pointer-width variable, passed as a parameter, and called through:
  the call lowers to an indirect call (`IR_CALLI` family). `var` parameters pass
  through. String and record (`sret`) returns through a value are not yet
  supported.
- Units (`unit` / `interface` / `implementation` / `uses`). A unit compiles on
  its own to `<name>.a`, a GNU `ar` archive carrying its object beside a
  reserved text member `.ppm` (a header line with the format version, unit name,
  target arch, and interface `uses` list, then the interface-section source
  verbatim). A `uses` consumer re-parses that source and injects the consts,
  types, and routine headings, so an exported record is laid out identically on
  both sides; the arch tag binds a unit to one target and a mismatch is refused.
  A used routine links by its plain name, so the linker pulls the unit's object
  member by symbol with no linker change, and the same `.a` links into a C
  program as an ordinary library. Interface and implementation `uses` are
  resolved transitively (interface deps recorded in the header), with cycle
  detection and shared-dependency dedup. Not yet: overloaded exports, qualified
  or selective `uses`, wildcard `uses`, and a unit initialization section.

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

Files and I/O:

- `Text` files, typed files (`file of T`), and untyped `file`, each a
  runtime-owned control block the front end stores inline and passes by address.
  `Assign`/`Reset`/`Rewrite`/`Close`, `Read`/`ReadLn`/`Write`/`WriteLn` on a file,
  `Eof(f)`/`Eoln(f)`, and `Erase`/`Rename`. Typed files add the ISO file-buffer
  variable `f^` with `Seek`/`FilePos`/`FileSize`/`Get`/`Put`; untyped files add
  `BlockRead`/`BlockWrite`. A `file of T` with a managed (string/dynarray)
  component is rejected, since it would write heap pointers. `{$I+}`/`{$I-}`
  selects whether a failed operation traps or sets `IOResult`.
- Field-width output: `Write(x:w)` and `Write(r:w:d)` right-justify integers,
  strings and chars, and print reals in fixed or scientific form.
- `for x in a do` over a static array, and `case` on a string selector.

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
`make test-pc-units` (ColdFire, in `make check`) and `test-pc-units-x86-64` (in
`check-all`) build a leaf unit, a unit that uses it in its interface, and a
program that uses both, then run the program: a unit exports a const, a record
type, and routines, with a transitive interface dependency and a diamond that
injects the shared unit once. The two lanes prove units are not ILP32-bound.

`make test-pc-gc-sections` (ColdFire, in `make check`) proves the linker
dead-strip: `skj-pc -ffunction-sections` puts each function in its own
`.text.<fn>` section, and a `skj-ld --gc-sections` link drops a function the
program never calls, producing a smaller binary that still runs correctly. The
`-ffunction-sections` flag is off by default, so ordinary output is unchanged.
See `doc/tools/toolchain.md` for the whole pipeline.

## Roadmap

The direction is a general-purpose Pascal large enough to compile real programs,
covering more of the ISO Extended Pascal and Free Pascal procedural core as
programs need it. Candidate areas, pulled in when a real program calls for them:

- A broader standard-routine set and wider ordinal and set coverage.
- Richer subrange and variant handling.

Out of scope, by design: the Delphi and Object Pascal object model (classes,
objects, interfaces, properties on classes, RTTI, exceptions as an object
hierarchy). The extensions here stay procedural and FPC-compatible, not
Object Pascal.
