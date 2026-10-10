# Kobold-Pascal Units and Separate Compilation

This note describes the design of the unit system: how a script is split across
several source files that compile separately and link together, so game scripts can
share and link against common material instead of living in one translation unit.

> Status: implemented. `kpcom` compiles a `program` or a `unit` separately to a KPM1 module and links precompiled units through `uses`, resolved by the `-U` search path and interface fingerprints; cross-unit routines, methods, types, and exported globals link and run, `implement I for R` trait conformance is checked, and `kpbundle` packages a program with its units into one self-contained module. A single translation unit plus `{$I}` includes still works as before. The surface and rules below match what the compiler accepts, settled in the decision log of `kobold.md`; the KPM1 framework this builds on is in `kp-module-format.md`, and the grammar is in `kp-grammar.md`. The `property` feature that earlier drafts folded in here has been split into its own proposal, `kp-property.md`; it is judged on its own merits and is not required for units, though it shares object-format inlining machinery with them (see Mapping to KPM1).

## What a unit is

A source file is either a `program`, which has an entry point, or a `unit`, which is a
library of declarations that other units and programs compile against and link to. A
unit has an `interface` part and an `implementation` part:

```pascal
unit geometry;
interface
    type Vec = record x, y: real end;
    function len(const v: Vec): real;
implementation
    function len(const v: Vec): real;
    begin
        len := sqrt(v.x * v.x + v.y * v.y)
    end;
end.
```

A name is exported by appearing in the `interface` part. Its body lives in
`implementation`. Anything declared only in `implementation` is private to the unit.
This is the Borland-family split. It was chosen over per-declaration export markers and
over a two-file definition/implementation layout because it keeps the exported surface
readable in one place while staying a single file per unit.

A unit has no `initialization` or `finalization` section. Run-once setup is a deferred
question, and auto-run unit setup is hard for an end user to reason about (link order,
partial init on error). Setup is an exported routine the program or host calls
explicitly. See the decision log in `kobold.md`.

## Opaque types

A unit may export a record type by name only, hiding its representation. The interface
lists the name; the implementation repeats the type with its full definition:

```pascal
unit grid;
interface
    type Grid;                          { opaque: name only, fields hidden }
    function make(w, h: integer): Grid;
    procedure put(var g: Grid; x, y: integer; v: byte);
    function clone(const g: Grid): Grid;
implementation
    type Grid = record
        w, h: integer;
        cells: array of byte
    end;
    ...
end.
```

An opaque type is a value record with hidden fields, not a reference. The compiler
computes the record's size and alignment from the implementation definition and writes
them into the KPM1 interface (see [Mapping to KPM1](#mapping-to-kpm1)); the fields
themselves stay private. A consumer therefore knows how much space the value occupies and
can hold it, pass it, and assign it by value, but it cannot name or touch a field. It
reaches the contents only through the unit's routines and methods.

The size is computed, never written by hand. Earlier exploration considered a
user-declared size (`OPAQUE RECORD(32)`); that is rejected as fragile, because a
hand-written size drifts out of step with the real definition. The definition in the
implementation is the single source of truth, and the compiler derives the size from it,
so the interface size is always correct.

Because the exposed size is baked into consumers (a value of the type sizes their stack
frames and enclosing records), it is part of the interface fingerprint: change the
record's layout and its size changes, so a consumer that allocates the type recompiles.
See [Interface fingerprinting](#interface-fingerprinting).

Assignment copies the record by value, the same as any Kobold record; there is no
reference sharing and no hidden deep copy. A deep copy that also duplicates any managed
members (a string, a dynamic array, a handle) is an ordinary routine the unit provides,
since only the unit sees the fields:

```pascal
var g, h: Grid;
g := grid.make(16, 16);
h := g;             { a value copy of the record }
h := g.clone;       { a fully independent grid, managed members duplicated }
```

There is no compiler-generated deep-copy hook for a whole record. Copying a record copies
its members by their category: a value member (a dynamic array, a nested record) is
value-copied, and a reference member (an interface or process handle, a `slice`, a `string`)
is retained. So an opaque value record whose fields are themselves values copies
independently; if a field is a reference the copy shares it, and a `clone` the unit provides
is how a caller gets a fully independent value. See the value/reference split and the
record-assignment entry in `kobold.md`.

## Exported state: var

A unit exports mutable state as a `var`. Constants are not state and are described under
Constants below. Computed, validated, or accessor-backed state is the separate `property`
feature (`kp-property.md`), which units can use once it exists but do not depend on.

An exported `var` is a plain storage cell, readable and writable from other units by
default:

```pascal
unit audio;
interface
    var volume: integer;        { read and write from other units }
implementation
    ...
end.
```

At the ABI a `var` is a data symbol: a consumer's `x := audio.volume` and `audio.volume
:= 5` compile to a direct load and store against the address resolved at link. There is
no access control on a plain `var`. When a unit wants read-only exposure, validation, or
a computed value, it uses a `property` (see `kp-property.md`) instead. Converting a `var`
to a `property` later changes the ABI (a load becomes a call), so consumers recompile;
the use-site source does not change.

### Thread-local by default, `shared` to opt out

A mutable exported `var` is thread-local by default. Today there is one execution
context, so this is only a documented guarantee with no runtime cost, but it fixes the
model before threads exist: a global is per-context unless the author opts into one
shared instance with `shared`:

```pascal
var score: integer;             { one per execution context }
shared var highScore: integer;  { a single instance across all contexts }
```

A `shared` global becomes the author's synchronization responsibility once real threads
exist. A `const` needs no such marking: it is immutable, so a single copy is always safe
to share. See [Globals: addressing and the ABI](#globals-addressing-and-the-abi) for how
the per-context model is implemented.

## Constants

An exported `const` is not state. It has no storage, no location, and no identity: it is
a name bound to a value. It has more in common with a preprocessor symbol or a linker
symbol than with a `var`, which is a cell with a reserved location. In the interface a
const lives next to exported type names and enum members, which are compile-time facts
carried as symbols in the same way, not next to `var`.

### Inline constants (the default form)

By default an exported `const` is compile-time inline. Its value is read from the
exporting unit's interface at consumer compile time and baked in, so it works everywhere
Pascal expects a compile-time constant, in array bounds, case labels, subrange types,
and other constant expressions:

```pascal
{ unit rules }
const MaxLives = 3;

{ consumer }
var lives: array[1..MaxLives] of Life;   { ok: compile-time }
case n of MaxLives: ... ;                 { ok }
```

Because the value is baked into consumers, changing an exported inline `const` requires
recompiling the units that use it, not just relinking. This is the same contract as a C
header constant or an enum member, not a pitfall to design around. It is made safe by
the interface fingerprint (see [Interface fingerprinting](#interface-fingerprinting)): a
consumer that baked the old value is rejected at link, not left running with a stale one.
Enum members are compile-time constants in the same way, carried with their type.

### Link-time constant symbols

A value that should change with only a relink, without recompiling consumers, is a
link-time constant symbol: the consumer emits a reference to a named const that the linker
patches to an immediate. Because its value is unknown at compile time, it cannot appear in
a constant position (an array bound, a case label, a subrange), the same boundary a property
has. It is the honest form for a tunable scalar in a foundation library, and it is cheaper
than an accessor property, which would add a routine and a call per read. It is a `const`,
not a property. Earlier drafts suggested expressing a changeable value as a read-only
property; that is retracted, because a plain scalar constant does not need an accessor.

The surface distinguishes it from an inline const by leaving the value out of the interface.
A unit declares a link-time const value-less in its interface, giving only its type, and
supplies the value in the implementation, the same shape an opaque type uses:

```pascal
unit config;
interface
    const Version: integer;      { link-time: a type, no value }
implementation
    const Version = 3;           { the value, resolved at link }
end.
```

Because the interface carries no value, a consumer sees only a typed name with no
compile-time value: it references the constant by address, resolved at link, and it cannot use
the name in a constant position. Changing the value and recompiling the defining unit changes
neither the name nor the type, so the interface fingerprint is unchanged and consumers relink
without recompiling. The value must be an ordinal or `int64` scalar; a string or other managed
value is not a link-time const (that waits on the flat-string work). Within the defining unit
the value is known, so the unit inlines its own uses; only cross-unit references go through the
symbol. The constant lowers to an exported read-only global, resolved through the same
exported-global table a cross-unit `var` uses. See [Mapping to KPM1](#mapping-to-kpm1).

## The uses clause

A program or unit names its dependencies in a single `uses` clause, at the top of the
file after the `unit` or `program` header. There is no separate interface-level and
implementation-level `uses`: a unit has one dependency list, private by default, and the
public surface is stated explicitly with `reexport`. This decouples what a
unit depends on from what it exposes, which the Borland interface/implementation split
conflates.

Every import has a qualifier. An unqualified `uses` uses the unit's own name as the
qualifier:

```pascal
uses geometry;              { names reachable as geometry.len }
uses geometry as geo;       { qualifier shortened to geo.len }
uses geometry as _;         { no qualifier: names come in bare, len }
```

The `as` slot sets the qualifier. The identifier `_` in that slot means "no qualifier",
so exported names come into scope bare. This makes qualification explicit at every
import and gives the bare form one clear spelling rather than an implicit default.

An optional selective list narrows which names are imported and may rename them
per item:

```pascal
uses geometry (Vec, len);              { only Vec and len, qualified as geometry }
uses geometry as geo (Vec, len);       { only Vec and len, qualified as geo }
uses geometry as _ (Vec, len as glen); { bare, and len is renamed to glen }
```

Without a list, all exported names are visible under the qualifier. With a list, only
the named items are visible; a rename sets the local name (bare when the qualifier is
`_`, otherwise under the qualifier).

## Re-export and cross-unit resolution

Two different questions arise when unit A's interface refers to a name from unit B, for
example a routine `A.makeThing: B.Thing`:

- **Resolution.** Can a consumer's compiler type-check `A.makeThing` and the value it
  returns? Yes, always. B is in the dependency closure, so kpcom reads B's interface to
  resolve `B.Thing`, whether or not the consumer names B. A consumer can hold and pass a
  `B.Thing` it got from A without knowing B, because the value carries its type.
- **Visibility.** Can the consumer *write* the name `B.Thing` itself, to declare a
  variable of that type or construct one? Only if B is in the consumer's own `uses`, or A
  re-exports it.

Re-export is explicit and selective. A unit promotes chosen names from a dependency into
its own public surface with `reexport`:

```pascal
unit scene;
uses geometry;
interface
    reexport geometry (Vec);          { consumers of scene can name Vec }
    function origin: Vec;
implementation
    ...
end.
```

A consumer of `scene` can now write `scene.Vec` (or `Vec` under its chosen qualifier)
without a separate `uses geometry`. Re-export is selective on purpose. Pulling a whole
dependency into the public surface is the transitive-surface bloat that has caused
trouble in other module systems, so a unit re-exports only the names its own interface
genuinely requires a consumer to name.

This is close to Rust's `pub use`. Classic Modula-2 has no such convenience: a client
imports every module whose names it writes, relying only on automatic resolution.
Kobold keeps that automatic resolution and adds selective re-export as the curated
middle.

## Dependency graph

The `uses` graph must be acyclic. Two units may not depend on each other, directly or
transitively. A cycle is a compile error. A DAG gives one resolution order, makes
separate compilation straightforward, and keeps the link step simple. When two units
seem to need each other, the shared declarations belong in a third unit both depend on.

## Methods across units

Methods are open. Any unit may attach a receiver method to any type it can see,
including a type imported from another unit:

```pascal
uses geometry;

{ add a method to geometry's Vec from here }
function taxicab for (const v: Vec): real;
begin
    taxicab := abs(v.x) + abs(v.y)
end;
```

A method is identified by the triple `(receiver type, method name, parameter
signature)`. Methods of the same name but different signature are distinct, so an
overload set for one `(type, name)` may span several units and is resolved by argument
type at the call site, the same as free-routine overloading.

### Definition uniqueness: the COMDAT-style fold

A definition can reach the linked program more than once. The `uses` graph is a DAG, so
a diamond is normal: unit A uses C, unit B uses C, and a program that uses A and B pulls
in C through both paths. Depending on how libraries are packaged, C's definitions may
arrive at the link once (C shipped as its own object, both A and B importing it) or twice
(A and B each a self-contained bundle that absorbed C). The fold makes both correct
without the program author choosing a packaging model, which is exactly why traditional
linkers carry COMDAT and weak-symbol folding.

Every definition absorbed from a used unit is foldable, in the spirit of a linker
folding COMDAT or weak symbols. A unit's own definitions are unique. The fold covers
every exported kind that can arrive twice through a dependency path, not methods alone:
routines, methods, types, and const symbols.

- Maintain a table keyed by symbol identity. For a free routine that is its qualified
  name and signature; for a method the triple `(receiver type, name, parameter
  signature)`; for a type its name; for a const its name.
- On the first definition of an identity, store it as canonical and record its source
  location and origin unit.
- On a later definition of an identity already present, compare the two. If they are
  **identical**, drop the later one and keep the canonical one. If they **differ**, it
  is an error that names both definition sites and their origin units.

Two definitions are identical when their signatures are structurally equal (the same
`SigEqual` / `SameParamType` test used for overloading) and their compiled bodies are
identical (compared by a body hash, with a full body compare on a hash match). A const
identity is identical when its type and value match. Source text is not the comparison:
whitespace and comments do not matter, the emitted code does.

In the common case the two copies of C came from the same compiler over the same source,
so they are byte-identical and the hash matches at once; the fold is cheap. The fold
earns its keep on version skew: A built against C v1 and B against C v2, whose bodies
differ, is an error that names both, not a silent choice of one. This is where the design
departs from Go, which would carry several versions of C in one program and treat them as
unrelated.

This is order-independent by construction. Whatever order definitions are visited in,
the result is the same: one canonical body, or an error. "Keep the first" is therefore
not a real choice, it is a fold, and it is safe only because a differing redefinition is
an error rather than a silent overwrite. That invariant, **duplicate implies identical
or error**, is load-bearing. It must never relax to "take the first even if different",
which would reintroduce a silent wrong-body hazard.

Because the fold is order-independent, the check may run at whatever point in
compilation or linking is convenient. It does not require a distinct whole-program
analysis phase. A shared symbol table maintains the invariant as definitions arrive,
which is what the backend already does for host imports.

The idiom for a genuine clash follows from the DAG: if two units both need `.area` on
`Vec` and their bodies differ, move the one intended definition into a common unit both
`uses`. The identical-fold case also means the same definition reaching the table twice
through a shared dependency is harmless, not an error.

This is implemented. The module format marks an absorbed definition `from <unit>` on its
`.func` or `.global` (see `kp-module-format.md`), and the loader folds two `from`-marked
copies of a symbol that meet at link: identical folds to one, differing is a link error
naming both origin units. A module's own definitions are never folded, so two unrelated
modules keep their own same-named exports. The `kpbundle` tool is the packaging step that
produces the absorbed bundles the fold operates on.

The fold and the interface fingerprint are complementary. The fingerprint catches
interface-level skew early, when a consumer resolves against a dependency it was not
compiled against; the fold catches body-level skew at the link, when A's copy of a
definition and B's copy meet. Both enforce the same "no silent version skew" invariant at
different stages.

### Method visibility and dispatch

A method is not gated by a selective import list or a qualifier. Selective lists and
qualifiers control free names (routines, types, constants). A method stays
dot-on-receiver: a call is written `v.area`, never `geo.area(v)`. It resolves whenever
its defining unit is anywhere in the program's dependency closure. Since a method
identity has a single definition program-wide, there is no per-call ambiguity to
resolve with a qualifier.

Dispatch itself is unchanged from the single-unit language, and Kobold has no dynamic
dispatch (see the no-dynamic-dispatch entry in `kobold.md`). On a **record** receiver a
method call resolves statically to the routine that implements it, so record-method
uniqueness is a compile-time namespace fact with no runtime table. On a **handle** receiver
the call resolves at link to the host's registered method; there is one implementation per
handle type, so a shadowing script method is resolved statically and the fold rule keeps that
one canonical body. See [Methods and dispatch](kobold.md#methods-and-dispatch).

Binding a call site to the right overload needs the complete overload set for its
`(type, name)`, so call-site method binding is resolved once the method table is
complete. That naturally happens at link, the same place host imports resolve. This is
ordinary deferred resolution, not a bespoke whole-program pass. Definition uniqueness
folds incrementally; call binding resolves once, at link.

## Interface fingerprinting

Separate compilation lets a consumer be compiled against one version of a unit's
interface and then linked against a different one. Whole-program compilation cannot drift
this way; separate compilation can, so the interface carries a fingerprint and the loader
checks it. This is a required part of the design, not an optimization.

Each object `.kpm` records, for every unit it was compiled against, the fingerprint of
that unit's interface as the consumer saw it. A unit's own `.interface` records its
current fingerprint. At link the loader compares the fingerprint a consumer expects for a
dependency with the dependency's actual fingerprint. A mismatch is a diagnostic that
names the stale consumer and the dependency, never a silent link against the wrong
interface.

The fingerprint covers the parts a consumer bakes in or binds to: exported routine and
method headings, exported type names and any transparent definitions, exported const
names and values (an inline const is copied into the consumer), exported `var` symbols and
their types, and any inlinable body a consumer may inline across the boundary (an inline
const value now, a small accessor body once property lands). A change a consumer cannot
observe, for example a private implementation-only routine or an unexported helper, does
not change the fingerprint and does not force a relink.

This is the safety mechanism behind the inline-const contract: an exported inline `const`
that changes value changes the fingerprint, so a consumer that baked the old value is
rejected at link rather than running with a stale number. It also bounds incremental
rebuilds: a consumer must recompile only when a dependency's fingerprint changes, not on
every private edit.

## Mapping to KPM1

A unit compiles to a KPM1 `object` module: `.module object`, with unresolved imports for
the units it uses. Its `.interface` section carries:

- exported routine and method headings (bodies go in the `functions` section);
- exported type names: a transparent type carries its full definition; an opaque record
  type carries its compiler-computed size and alignment but not its fields, so a consumer
  can allocate and value-copy it without seeing the layout;
- exported constants: an inline `const` as a symbol with its value (`.const <name> <type>
  <value>`), since it is compile-time and a consumer bakes the value in; a link-time const
  value-less (`.const <name> <type>`), its value carried instead by an exported read-only
  global outside the interface (so it is not in the fingerprint), which the consumer references
  by address through the exported-global table, exactly like a cross-unit `var`;
- exported `var` symbols, as data symbols with a type and a `shared` flag;
- the interface fingerprint (see [Interface fingerprinting](#interface-fingerprinting)),
  plus, per dependency, the fingerprint the unit was compiled against;
- exported `property` entries once the `property` feature lands, each carrying its
  accessor symbols and, where the accessor is small, an inlinable body; until then the
  interface has no property entries (see `kp-property.md`).

A consumer type-checks against the `.interface` alone and records the fingerprint it saw
for each dependency. The linker reads the `functions` bodies to fold definitions and to
resolve calls and data symbols.

The interface can carry inlinable bodies, not only signatures. An exported inline `const`
already puts its value in the interface for the consumer to bake in; the same mechanism
generalizes to small routine bodies, so a trivial accessor can be inlined across the unit
boundary. This is the object-format hook that lets a `property` read lower to look like a
plain `var` access instead of a call, and it is why property and units were originally
designed together (see `kp-property.md`). An inlined body is consumer-observable, so it is
part of the interface fingerprint, exactly like an inline const value: change the body and
the consumers that inlined it must recompile.

A program compiles to a KPM1 `executable` module: `.module executable` with `.entry
main`. See `kp-module-format.md` for the module kinds and the interface section that
`kpcom` produces and the loader consumes.

For good diagnostics, an exported method heading in the `.interface` records its
defining unit and source location, so the fold rule can point at both sites when two
definitions clash.

## Compilation and linking model

Linking happens in the backend, at load. The split is:

- **Compile against interfaces.** kpcom compiles one unit at a time. It reads the
  `.interface` sections of the units in the dependency closure to resolve and type-check,
  and it does not need their `functions` bodies. It records the fingerprint it saw for
  each dependency and emits an `object` `.kpm` for the unit.
- **Link objects at load.** The backend loads a set of `object` `.kpm` modules and links
  them into a runnable form: it checks each consumer's expected dependency fingerprints,
  folds definitions, resolves call sites and data symbols across modules, binds dispatch,
  and lays out globals. This reuses the load/link and symbol-table machinery that already
  resolves host imports.

So a `.kpm` is read at two stages: by kpcom for its interface when compiling a dependent
unit, and by the backend for its bodies when linking a runnable set. This is what makes a
precompiled `library` `.kpm` usable by a third party with no source, and it is why the
`library` and `executable` module kinds in `kp-module-format.md` pay off. The method fold
rule keeps the backend's new guest-to-guest resolution cheap, because it is an insert
into the existing symbol table (see [the fold rule](#definition-uniqueness-the-comdat-style-fold)).

### Building and running a unit set

The runner links the set. `kprun` loads a program `.kpm`, reads its `.uses` list, and loads
each dependency `.kpm` by name from the program's directory, following each dependency's own
`.uses` so a transitive unit is loaded once. It then links and runs the whole set in one
`kp_link`. An embedding host does the same through the public API: `kp_module_dep_count` and
`kp_module_dep_name` walk a loaded module's dependencies, and `kp_module_name` tells which
units are already loaded. There is no separate build tool. The compile step is
`kpcom --emit-kpm -U<dir>` per unit, and the recompile-only-on-interface-change property is a
build-system concern: extract each object's `.interface fingerprint` to a stamp file rewritten
only when it changes, and make a consumer depend on that stamp rather than the dependency's
whole `.kpm`. `examples/units` is a worked Makefile that does exactly this, and it doubles as
the end-to-end multi-unit test in `make check`.

The alternative packaging model is a self-contained bundle. `kpbundle <out> <program> <unit>...`
absorbs the units into the program module, dropping their `.uses`/`.interface` and tagging each
absorbed definition `from <unit>`, so the bundle runs on its own with no unit files beside it.
Two bundles that each absorbed the same unit fold that unit's definitions at link (see the fold
above). The `examples/units` Makefile builds and runs both the object set and the bundle.

### The unit search path

A unit is found by name at two stages, and both stages search the same order: explicit
directories first, then the installed standard library.

- **Compile time.** `kpcom` resolves each `uses` name to an interface `.kpm` by searching, in
  order, every `-U<dir>` given on the command line, then a directory baked in at build time
  (the install location, `$(PREFIX)/lib/kobold`). A name is tried as `<dir>/<name>.kpm` and
  then `<dir>/<name>.kpm` lowercased.
- **Run time.** `kprun` resolves each dependency in the `.uses` list by searching, in order,
  the main module's own directory, every colon-separated entry of the `KOBOLD_UNITS`
  environment variable, then the same baked-in standard library directory. An embedding host
  that walks dependencies with `kp_module_dep_name` chooses its own lookup.

So a program that `uses strings` compiles and runs with no flags once the library is installed,
builds against a local copy with `-Ubuild/lib`, and runs against a local copy with
`KOBOLD_UNITS=build/lib`. The lowercase fallback keeps a `uses Strings` working on a
case-sensitive filesystem where the file is `strings.kpm`.

### The standard library

The standard library is a set of ordinary units installed under `$(PREFIX)/lib/kobold`, both as
source (`.pas`) and as a precompiled interface (`.kpm`). It uses no privileged mechanism: a
standard-library unit is found and linked exactly like any other unit on the search path, and a
program could supply its own unit of the same name ahead of it with `-U` or `KOBOLD_UNITS`.

The first unit is `strings`: pure string helpers (`UpperCase`, `LowerCase`, `Trim` and its
one-sided forms, `StringReplace`, and string `Delete`/`Insert`) built on `Copy`, `Pos`,
`Length`, and concatenation, with no runtime support of its own. Because strings are immutable,
its `Delete` and `Insert` return a new string rather than mutating one. They share a name with
the dynamic-array `Delete`/`Insert` built-ins, so a program reaches them qualified, as
`strings.Delete`, where they do not shadow the built-ins. `examples/strdemo.pas` is the worked
example, run end to end in `make check`.

## Globals: addressing and the ABI

Exported globals are addressed base-relative, not by absolute address. Each module has a
globals frame; a global access is `[base + offset]`, where the offset is a constant fixed
at compile time within the module and the base is provided at load. There are two frame
classes:

- a **per-context** frame for thread-local globals (the default), reached through a
  per-context base the loader supplies;
- a **shared** frame for `shared` globals, reached through one process-wide base.

This choice is not only about future threads. Because each access offset is a
compile-time constant relative to its frame, a cross-module global reference needs no
load-time relocation, unlike an absolute address that is unknown until load. The loader
sets one base per frame instead of patching every access site, so modules are cleanly
relocatable, which suits the load-time backend linking above. Thread-local then costs
nothing extra later: each execution context gets its own per-context base, and `shared`
globals keep their single frame. Whether the base lives in a pinned register or is loaded
on demand in functions that touch globals is an emitter detail, deferred.

## Future directions

- **A cell as a first-class value (option 8).** Shared mutable state that wants identity
  and, later, synchronization could be modeled as a `ref`/cell handle created and passed
  around, built on the existing handle and ARC machinery with no new object-format work.
  This is the Rust `Arc<Mutex>` or Clojure atom shape. It is less "looks like a global"
  than an exported `var`, but it is the honest home for genuinely shared, synchronized
  state. Worth investigating as an alternative or complement to `shared var`.
- **The `property` feature.** Uniform-access, accessor-backed, and computed values,
  including computed record fields, are specified in `kp-property.md` as an independent
  proposal. Units can use it once it lands but do not require it.

## Open questions

- **Whether `kpcom` itself is ever split into units.** The compiler is a single Pascal
  file today, and the self-hosting fixed point does not depend on units. Splitting it is
  not proposed here.

## See also

- `kp-grammar.md` the concrete syntax for units and the `uses` clause.
- `kp-property.md` the split-out property / uniform-access proposal.
- `kp-module-format.md` the KPM1 module kinds and interface section.
- `kobold.md` the language and runtime design, and the decision log.
- `kp-conformance.md` where units sit against the Pascal standards and dialects.
