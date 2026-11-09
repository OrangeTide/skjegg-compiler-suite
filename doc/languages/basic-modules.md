# BASIC Modules

A way to split a BASIC program across files and to build and consume shared
libraries, in the Modula-2 tradition. The import surface is `FROM M IMPORT a, b`
and `IMPORT M`; a library is a source file headed by `MODULE M`. This document is
the specification; it is ahead of the implementation, and the status section at
the end says what is built. The core language is `basic.md`; the shared on-disk
container is the `ar`-archive unit format that Kobold already uses
(`kobold/kp-units.md`), which this reuses without sharing any metadata with it.

## Design in one paragraph

A module on disk is one GNU `ar` archive (`.a`). It holds ordinary relocatable
objects (`.o`), which are the real code and are language neutral because every
backend emits the platform psABI, plus one reserved metadata member per language
that binds to those objects. BASIC's member is `.bmi` (BASIC module interface);
Kobold's is `.kpm`. The per-language members are independent: BASIC never reads
`.kpm`, Kobold never reads `.bmi`, and neither is a shared contract the other must
honour. This is what lets one archive carry a BASIC wrapper and a Kobold wrapper
over the same C foundation, each shaped for its own language, each pulling only
the objects a program actually references.

## What a module is

A BASIC source file is either a **program**, which runs, or a **module**, which
is a library of declarations other programs compile against and link to. A file
is a module when its first statement is `MODULE`:

```basic
MODULE Geometry
```

A module has no top-level executable statements and no entry point. Setup, when a
library needs it, is an exported routine the program calls; there is no automatic
initialization. (This follows the Kobold-unit rule rather than Modula-2's
auto-run module body, which may be added later.)

The interface a module presents is exactly the set of its `EXPORT`ed
declarations. There is no separate definition file to write and keep in step: the
compiler derives the interface and writes it into the archive as the `.bmi`
member. That member is the machine-generated equivalent of a Modula-2 definition
module.

## Exporting

A declaration is part of the module's interface when it is marked `EXPORT`.
Anything not marked `EXPORT` is private to the module.

```basic
MODULE Geometry

EXPORT CONST MaxSides = 32

EXPORT TYPE Circle
    cx AS FLOAT
    cy AS FLOAT
    r  AS FLOAT
ENDTYPE

EXPORT FN Area(c AS Circle) AS FLOAT
    Area = PI * c.r * c.r
ENDFN

EXPORT PROC Scale(REF c AS Circle, k AS FLOAT)
    c.r = c.r * k
ENDPROC
```

The exportable kinds are `CONST`, `TYPE`, `FN`, and `PROC`. A module variable is
private; shared mutable state is reached through exported accessor routines, not
an exported global.

### Exported types are opaque

An `EXPORT TYPE` is **opaque** to importers. The importer receives a handle, a
single pointer-width token, and never sees the fields or the layout. Fields are
reached only through routines the module exports, which is the property and
accessor model: a field read is an exported `FN`, a field write an exported
`PROC`, and construction an exported `FN` returning the handle.

```basic
EXPORT TYPE Circle         ' opaque to importers
    cx AS FLOAT
    cy AS FLOAT
    r  AS FLOAT
ENDTYPE

EXPORT FN NewCircle(x AS FLOAT, y AS FLOAT, rad AS FLOAT) AS Circle
EXPORT FN CircleR(c AS Circle) AS FLOAT          ' getter
EXPORT PROC SetCircleR(c AS Circle, v AS FLOAT)  ' setter
```

Keeping the layout private is what makes a type portable: no importer, in any
language, depends on field offsets, so nothing has to reconcile layouts between
languages. Inside the module the type is an ordinary BASIC record with real
fields; only the exported surface is a handle.

## Importing

A program brings a module's exports into scope with either Modula-2 form:

```basic
FROM Geometry IMPORT Circle, Area, NewCircle     ' selective, unqualified
IMPORT Geometry                                  ' qualified
```

- `FROM M IMPORT a, b` binds the listed names unqualified, so they are used bare
  (`Area(c)`).
- `IMPORT M` binds the module qualified, so its names are used dotted
  (`Geometry.Area(c)`). The `.` separator is the same token a record field access
  uses (`c.r`); a reference is read as module-qualified only when its leading
  name is an imported module, otherwise it is a field access, so the two never
  collide (nor with the `$` string-name suffix).

An imported `FN`/`PROC` has no body in the importer; a call lowers to the same
psABI `CALLI <linksym>` BASIC already emits for runtime calls. An imported
`CONST` is a compile-time value and is inlined, so it needs no symbol. An imported
opaque `TYPE` is a handle, used only as an argument or result and through the
module's accessors. Either import form carries opaque types: a handle variable is
declared `DIM c AS Circle` for a `FROM M IMPORT` type, or `DIM c AS M.Circle` for
a qualified `IMPORT M` type (the same dotted form the module's routines take).

The compiler finds a module named `M` by searching an import path for `M.a`,
then reads that archive's `.bmi` member. The path is given with repeated `-I
<dir>` driver options, searched in order, with the current directory as a final
fallback. The `.bmi` carries the target width and endianness, and the compiler
rejects a module built for a different target rather than miscompiling its
calls.

## On-disk form: the archive

A compiled module is a GNU `ar` archive, the same container Kobold units use
(`kp-units.md`). Reserved metadata members have short, dot-prefixed names that the
archiver does not index and a symbol-driven linker never pulls; they are found by
exact name. Object members are symbol indexed, so the linker pulls only the ones a
program references.

```
libgeom.a
  .bmi                 BASIC module interface (this document)
  geom_bas.o           the module's BASIC code
  geom_c.o             an optional C foundation this module wraps
  .kpm                 an optional, independent Kobold binding to the same code
  geom_kp.o            the Kobold wrapper's code
```

`.bmi` is deliberately not an ELF object and carries no symbol table, so `skj-ar`
does not index it and the linker ignores it. A program module (one that runs)
stays a bare program and is not wrapped; the archive form is for a library, the
thing other code resolves by name.

### Building in stages

Members are added to one archive over several independent steps, which is how a
multi-language library is assembled. Each step appends; no step reads or rewrites
another step's metadata.

```sh
# a C foundation
skj-cc -c -o geom_c.o geom.c
skj-ar rcs libgeom.a geom_c.o

# a BASIC wrapper over it: MODULE source -> code object + .bmi
skj-bc --emit-module --meta geom.bmi -o geom_bas.o geomwrap.bas
skj-ar r libgeom.a geom_bas.o geom.bmi

# later, independently, a Kobold wrapper over the same geom_c.o
skj-kpc --emit-unit ... geomwrap.kp
skj-ar r libgeom.a geom_kp.o geom.kpm
```

A pure-BASIC library omits the C object. The archive now serves C through a
header, BASIC through `.bmi`, and Kobold through `.kpm`, all sharing `geom_c.o`,
and each program links lean.

## The `.bmi` metadata format

`.bmi` is a small, line-oriented text member, interface only. BASIC does not
inline across modules, so unlike `.kpm` it carries no IR, only signatures and the
link symbols they bind to. Lexical rules match `.kpm`: ASCII, LF-terminated lines,
tokens separated by whitespace, `;` begins a comment, keywords case-insensitive,
symbol names case-sensitive.

```
.bmi 1
.target lp64 little
.module Geometry

.const  MaxSides  i64 = 32

.type   Circle    handle
.ctor   Circle    link geom_new_circle   ( f64 f64 f64 ) -> handle Circle
.getter Circle.r  link geom_circle_r      ( handle Circle ) -> f64
.setter Circle.r  link geom_set_circle_r  ( handle Circle f64 )

.func   Area      link geom_area          ( handle Circle ) -> f64
.proc   Scale     link geom_scale         ( handle Circle f64 )
```

- `.bmi <version>` is required and first. `.target <width> <endian>` is
  authoritative, `ilp32`/`lp64` and `little`/`big`; the reader rejects a module of
  the wrong width or endianness rather than miscompiling a call. A module is per
  target, so a project builds `libgeom-rv32.a`, `libgeom-x86-64.a`, and so on.
- `.module <Name>` gives the name used for qualified import.
- Value classes are primitives only: `void`, `i8` `i16` `i32` `i64`, `f32` `f64`,
  `str` (a BASIC string handle), and `handle <Type>` (an opaque exported type).
  BASIC's own types map on as `INT` to `i64`, `BYTE` to `i8`, `FLOAT` to `f64`,
  `STRING` to `str`, and an opaque `TYPE` to `handle <Type>`. There are no
  by-value records or arrays in a signature; aggregates cross as a handle with
  accessors.
- A handle names its opaque type, written `handle <Type>`, where `<Type>` is a
  `.type` declared in the same `.bmi`. The name makes the handle nominally typed:
  the importer rejects a call, assignment, or return that mixes two opaque types,
  the same check the defining module applies to itself. A `handle` with no name
  is rejected.
- A `REF` primitive parameter is passed as a pointer, written `ref <class>` (for
  example `ref i64` for `REF n AS INT`); the caller passes an address. A record
  parameter is always `REF` and crosses as the opaque `handle <Type>` itself, so
  it needs no `ref` prefix.
- `link <sym>` is the exact object symbol an entry binds to, decoupled from the
  BASIC name, so a wrapper can bind to any underlying symbol (the same role as
  Kobold's `external name` alias).
- `.const` carries its value inline and defines no symbol. `.func` returns a
  value, `.proc` does not. `.ctor`, `.getter`, and `.setter` are the accessor
  surface of an opaque `.type`; a getter takes the handle, a setter takes the
  handle and the new value.

The current compiler emits an opaque type's accessors as ordinary `.func` and
`.proc` over a `handle`, which is already enough to construct, read, and mutate
an opaque value. The distinct `.ctor`/`.getter`/`.setter` records shown above are
a planned refinement that lets a tool and a C header pair the accessors with the
type automatically; the reader does not require them yet.

## Crossing to a C foundation, and to other languages

`str` is a BASIC string handle, a pointer to a length-prefixed byte block in the
shared runtime arena. A BASIC-to-BASIC call passes it directly, because both sides
link the same `basic_rt` and share the representation. Crossing to a C foundation
is the wrapper's job: the exported BASIC routine takes `STRING`, and its body
marshals to the foundation's `( ptr, i64 len )` call. This marshalling is why a
language's wrapper object exists, and why the BASIC and Kobold wrappers over one C
foundation differ while bottoming out in the same `.o`.

The accessor model is what makes an opaque type portable across all three
languages at once. The same `geom_new_circle` / `geom_circle_r` symbols are a
handle plus getters in `.bmi`, a `property` in `.kpm`, and in a C header a set of
functions with convenience macros:

```c
#define Circle_r(h)        geom_circle_r(h)
#define Circle_set_r(h, v) geom_set_circle_r((h), (v))
```

Nothing reconciles struct layout anywhere, because no layout crosses a boundary.

## Binding a C library directly: `EXTERNAL`

A routine can bind an existing C symbol with no BASIC body by declaring it
`EXTERNAL`:

```basic
EXPORT DECLARE PROC SetBg(r AS INT, g AS INT, b AS INT) EXTERNAL "psx_set_bg"
EXPORT DECLARE FN   Fmul(a AS INT, b AS INT) AS INT EXTERNAL "psx_fmul"
```

The string is the object symbol the call binds to (the `link` field of the entry),
so a tidy BASIC name can wrap a differently spelled C symbol. This is the BASIC
form of Pascal's `external` and Kobold's `external cdecl name`, and it is how the
libpsx binding (`units/psx.bas`) reaches the PlayStation SDK. An `EXTERNAL`
declaration may stand in a module, whose interface is then exported in the `.bmi`,
or in a plain program that links the symbol directly.

An `EXTERNAL` routine crosses `INT` at the C `int` width. A BASIC `INT` is 64-bit
but a C `int` is 32-bit, so an `EXTERNAL` routine's `INT` parameter and result are
the `i32` class in the `.bmi`: the compiler narrows each argument to 32 bits at the
call and sign-extends a 32-bit result back to `INT`. A BASIC-to-BASIC routine keeps
the native 64-bit `INT` (`i64`), so ordinary module calls are unaffected. The
boundary handles only the primitives the C ABI passes by value; a pointer or struct
argument stays on the C side behind an accessor that takes and returns handles and
scalars, as `units/psx.bas` and `examples/psx/sprite_asset.c` show.

## Linking and dead code

Because each language's code lives in its own object members and the linker pulls
by symbol reference, a program links only the members it uses. A BASIC program
that imports two routines from a large library drags in only the objects those two
routines reach. The later `--gc-sections` dead-strip work (see `PLAN.md`) sharpens
this from object granularity to per-function granularity, and modules are its
natural home.

## Known limitations

- **A `REF` parameter is not yet importable.** Both import forms reject a routine
  with a by-reference parameter (reported at its reference for `IMPORT M`, at the
  import for `FROM M IMPORT`). Every other signature, opaque handles included, is
  supported through both forms.

## Grammar additions

These join the core grammar in `basic.md` when implemented:

```
ModuleStmt = 'MODULE' IDENT .
ImportStmt = 'FROM' IDENT 'IMPORT' IDENT { ',' IDENT }
           | 'IMPORT' IDENT .
ExportDecl = 'EXPORT' ( ConstDecl | TypeDecl | ProcDecl | FnDecl | ExternDecl ) .
ExternDecl = 'DECLARE' ( ProcHeader | FnHeader ) 'EXTERNAL' STRING_LITERAL .
Qualified  = IDENT '.' IDENT .                 ' a qualified-import reference
```

New reserved words: `MODULE`, `IMPORT`, `FROM`, `EXPORT`. A `MODULE` statement,
when present, must come first. An import (`FROM` or `IMPORT`) appears at the top
level; by convention it is placed before the declarations that use it, though the
compiler accepts it anywhere among them, since a module name is resolved as the
import is read.

## Status

Partly implemented. The first implementation cut is BASIC only, since Kobold
already has its unit system and C is only the foundation:

1. **Done.** The `.bmi` reader and writer (`basic/bmi.c`), `skj-bc --emit-module`
   and `--meta <file>`, and `MODULE` with per-declaration `EXPORT` for `CONST`,
   `FN`, `PROC`, and `TYPE`. A `MODULE` compiles to an object with no entry point
   whose exported routines are global and whose private ones are file-local, plus
   the `.bmi` interface. Tests: `make test-basic-module` (the producer and the
   golden `.bmi`) and `make test-basic-bmi` (the reader).
2. **Done.** `FROM M IMPORT a, b` consumes a module archive's `.bmi` member: the
   driver resolves `M.a` on the `-I` import path, the reader checks the target
   tag, an imported `CONST` folds to its value, and an imported `FN`/`PROC` is
   registered as an external routine whose call lowers to the module object's
   link symbol (the linker pulls the object from the archive by symbol
   reference). The supported signatures are by-value primitives; a `REF`
   parameter or an opaque `handle` is rejected with a clear message, pending step
   3. Test: `make test-basic-import` (a program imports and calls across the
   archive boundary).
3. Split in two:
   - **Done (3a).** Qualified `IMPORT M` with dotted `M.name` references. `IMPORT
     M` brings every supported export into scope under its dotted name, resolved
     against the same archive. A reference `M.name` is read as module-qualified
     when `M` is an imported module (otherwise it is a record field access), with
     no lexer change: the existing `.` token carries it. A CONST resolves to its
     value, an FN/PROC to a call on the module's link symbol. An unsupported
     export (a `REF` parameter) is left out of a bulk `IMPORT M` and reported
     only where it is referenced, so a module stays importable for its other
     exports. Test: `make test-basic-import-qual`.
   - **Done (3b).** Opaque types as a handle with exported accessors. A module
     `EXPORT TYPE X` is a handle at the boundary; within the module a variable or
     parameter of that type is handle-backed (a `DIM` heap-allocates the record
     through the shared runtime arena, so a constructor can return it), and its
     fields are reached only inside the defining module. A constructor is an
     `EXPORT FN` returning the type, a getter an `EXPORT FN` taking it, a setter
     an `EXPORT PROC` taking it; none needs `REF` (the opaque type is implicitly
     by handle). An importer brings the type in with `FROM M IMPORT X, ...`,
     declares a handle variable (`DIM c AS X`), constructs through the module, and
     reaches the value only through the exported routines; a field access on an
     imported opaque type is rejected, because its fields are private. A qualified
     `IMPORT M` reaches the same opaque type through its dotted name: the handle
     variable is `DIM c AS M.X` and the accessors are `M.NewX`/`M.GetX`, with the
     opaque identity carried on the dotted key so the nominal check still holds.
     The getter/setter/constructor are ordinary `.func`/`.proc` over a `handle`,
     not yet the distinct `.ctor`/`.getter`/`.setter` records below (that sugar
     remains future work). Tests: `make test-basic-opaque` (FROM) and
     `make test-basic-opaque-qual` (qualified).

`str` is allowed in an exported signature: a BASIC-to-BASIC call passes the
string handle directly, since both sides share the runtime's representation (a
crossing to a C foundation is still the wrapper's job, as above). Whether an
exported module variable is ever allowed stays closed: no, a module reaches
shared state through accessor routines, not an exported global.
