# Native-backed optional units

A Kobold program reaches an external C library through a *native-backed unit*: an
ordinary `uses` unit whose implementation calls a thin C glue layer that in turn
calls the library. These units are optional: each builds only when its external
dependency is present, so a plain build of the toolchain needs none of them and a
program that does not `uses` one is unaffected.

The mechanism serves any C library. The first unit built on it is `std.osthreads`,
a shared-nothing OS-thread binding over the platform threads library.

## Enabling a unit: configure and config.mk

The optional units are off until `./configure` detects their dependencies:

```sh
./configure
```

`configure` probes each optional library and writes `config.mk` with a `HAVE_<LIB>`
flag and the library's compile and link flags. The Makefile reads `config.mk` with
`-include`, so a unit's build rules exist only when its dependency was found.
`config.mk` is generated and is not checked in; re-run `./configure` whenever the set
of installed libraries changes.

```sh
./configure --with-<lib>=no     # force a unit off even if the library is present
./configure --with-<lib>=yes    # fail if the library is not found (for pinning)
```

With a unit enabled, `make` builds it, `make check-all` runs its test, and
`make install` ships it alongside the standard library units. An installed
compiler finds a shipped unit on its own: it resolves its standard-library
directory relative to its own binary, as `<bindir>/../lib/skjegg/kobold/<target>/`,
so a `uses` needs no `KOBOLD_UNITS`. A unit built in the tree, or installed
somewhere else, is still reached by listing its directory in `KOBOLD_UNITS` or
with a `-U` flag.

## What a native-backed unit is made of

A native-backed unit is three things built into two installed files:

- the Kobold interface, `kobold/lib/<name>.pas`, which wraps the C glue in typed
  routines, exactly like any other unit;
- the C glue, `kobold/lib/<name>_glue.c`, compiled with the library's own compile
  flags;
- an external dependency, named by its link flags.

The build archives the compiled glue object **inside the unit's `.a`**, beside the
reserved `.kpm` interface member, and writes the external link flags to a sidecar
`<name>.link` file next to the `.a`. When a program `uses` the unit, the
ahead-of-time driver (`kobold/aot-run.sh`) extracts the glue object from the archive
and appends the sidecar's flags to the final link, so the glue's calls into the
library resolve.

```
osthreads.a    .kpm             the interface Kobold type-checks `uses` against
               osthreads_glue.o the compiled C glue, pulled into the link
osthreads.link -lpthread         the external link flags
```

## No pointers: objects are integer ids

Kobold holds no pointers, so a native object is named to the script by a small
integer id, never an address. The glue keeps a fixed table of the real objects and
hands back an index; a later call passes the id back and the glue resolves it,
bounds-checked. An invalid or freed id fails the call rather than dereferencing a bad
pointer.

## Callbacks: passing a Kobold routine to C

A Kobold procedure value is a bare code pointer emitted through the platform C ABI,
so a native-backed unit may hand a routine to its C library as a function pointer and
the library may call it back. Declare the callback type `cdecl` and pass the routine
with `@name` (see the "Procedure types" section of `kp-language.md`). `std.osthreads`
is built on exactly this: the thread body is a Kobold routine the glue runs on a real
OS thread.

## Ahead-of-time only

Native-backed units are ahead-of-time only, like inline assembly. The in-process JIT
binds host symbols from a fixed table and has no path to an external library's
symbols, so a program that `uses` such a unit must be compiled with `skj-kpc` and
linked, not run under `skj-kp-jit`. The JIT recognizes a native-backed unit by the
glue object in its archive and refuses it with a clear message rather than failing to
resolve the symbols:

```
skj-kp-jit: unit 'osthreads' wraps an external C library and can only be used in an
ahead-of-time build (compile with skj-kpc); the JIT cannot bind external symbols
```

Emitting a module with `skj-kp-jit --emit-kpm` still works, since that only compiles
and does not load the unit.

## Using the std.osthreads unit

`std.osthreads` runs a Kobold routine on a real OS thread, the preemptive counterpart
to the cooperative `std.threads`. It is the first unit built on the native-unit
mechanism, and it depends on procedure values crossing the FFI: the thread body is a
Kobold routine handed to the C glue as a function pointer.

Shared-nothing is the contract: a worker takes one integer and returns one integer, and
must not touch a string, array, map, or any managed value shared with another thread
(the runtime's reference counting and heap are single-threaded). Within that rule it
gives real parallelism for compute kernels.

```pascal
program parsum;
uses std.osthreads as _;

function work(n: integer): integer;    { pure integer, no managed state }
var i, s: integer;
begin
  s := 0;
  for i := 1 to n do s := s + i;
  work := s;
end;

var a, b, c: integer;                  { thread handles are integer ids }
begin
  a := Spawn(@work, 100);              { @work: a routine value, not a call }
  b := Spawn(@work, 1000);
  c := Spawn(@work, 10);
  writeln(Join(a), ' ', Join(b), ' ', Join(c));
end.
```

`Spawn(@work, arg)` starts `work(arg)` on a platform thread and returns a handle;
`Join` waits and returns the worker's result. `@work` takes the routine's value: a
parameterless-or-not worker named bare would be called, so the address operator is how
the routine itself is passed.

See `kp-units.md` for the unit model in general and `kobold.md` for the ahead-of-time
build.
