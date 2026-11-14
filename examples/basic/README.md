# BASIC module example

A self-contained "define a module, consume it" pair for the BASIC front end
(`skj-bc`). It is the runnable companion to the worked example in
[the BASIC module guide](../../doc/languages/basic-modules.md).

- `geometry.bas` is a `MODULE`: a library with no entry point. It exports a
  constant, an opaque `Circle` handle with its accessor routines, and two plain
  functions. The record fields of `Circle` and the private `Pi` helper stay
  inside the module.
- `shapes.bas` is a program that imports `Geometry` and uses it. It constructs a
  `Circle` through `NewCircle`, reads and mutates it only through the exported
  routines, and prints the results.

## Building and running

A module ships as an `ar` archive that carries both its compiled code and its
`.bmi` interface. The archive is named after the module, so `MODULE Geometry`
becomes `Geometry.a`, and the consumer finds it on the `-I` import path.

```sh
# 1. Compile the module to an object plus its .bmi interface.
skj-bc --emit-module --meta build/geometry.bmi -o build/geometry.s geometry.bas
skj-as -o build/geometry.o build/geometry.s

# 2. Stage the interface and the object into Geometry.a.
sh ../../tests/basic-archive.sh skj-ar build/Geometry.a build/geometry.bmi build/geometry.o

# 3. Compile the consumer, pointing -I at the directory holding Geometry.a.
skj-bc -I build -o build/shapes.s shapes.bas
skj-as -o build/shapes.o build/shapes.s

# 4. Link against the runtime and the module archive, then run.
skj-ld -o build/shapes build/start.o build/basic_rt.o build/shapes.o build/Geometry.a
qemu-m68k build/shapes
```

The toolkit wires this up as a Makefile target that builds the archive, links
the consumer, runs it under `qemu-m68k`, and checks the output against
`shapes.expected`:

```sh
make test-basic-example
```

That target is part of `make check`.

## Expected output

```
MaxSides=32
radius=5.0
diameter=10.0
area=78.53981625
after SetRadius:
radius=10.0
diameter=20.0
```

## The two import forms

`shapes.bas` uses the selective, unqualified form, which binds the named
entities as if they were local:

```basic
FROM Geometry IMPORT Circle, NewCircle, Radius, Diameter, Area, SetRadius, MaxSides
```

The alternative is to import the whole module and reach its entities under dotted
names:

```basic
IMPORT Geometry
DIM c AS Geometry.Circle
c = Geometry.NewCircle(1.0, 2.0, 5.0)
PRINT Geometry.MaxSides
```

Both forms carry the opaque `Circle` type with its identity intact, so a handle
from one import style is the same type as a handle from the other.
