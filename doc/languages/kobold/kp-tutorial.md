# Kobold-Pascal Tutorial

This tutorial teaches Kobold-Pascal from a first program upward. Each step is a
complete program you can compile and run. For the quickest loop, run each with
the in-process JIT:

```sh
skj-kp-jit step.pas
```

For the precise rules behind anything here, see the reference (`kp-language.md`)
and the grammar (`kp-grammar.md`). For how to embed the language in a C host,
see `kp-guide.md` and `kp-embedding.md`.

## 1. Hello, Kobold

Every program is a `program` block with a name, optional declarations, and a
body between `begin` and `end.` (note the trailing period).

```pascal
program hello;
var i: integer;
begin
  WriteLn('Hello from Kobold via skjegg');
  WriteLn('6 * 7 = ', 6 * 7);
  for i := 1 to 3 do
    WriteLn('line ', i);
end.
```

`WriteLn` is not special syntax; it is an overloaded routine, so it prints
strings and integers through one name and takes several arguments. Kobold is
case-insensitive, so `WriteLn`, `writeln`, and `WRITELN` are the same name.

Run it:

```sh
skj-kp-jit hello.pas
```

## 2. Variables and expressions

Declare variables in a `var` block, with the type after the colon. Assignment is
`:=`; `=` is the equality test. The ordinary integer operators are `+ - *`, with
`div` and `mod` for integer division and remainder; `/` produces a `real`.

```pascal
program vars;
var
  a, b, sum: integer;
  avg: real;
begin
  a := 17;
  b := 25;
  sum := a + b;
  avg := sum / 2;
  WriteLn('sum = ', sum);
  WriteLn('avg = ', avg);
end.
```

## 3. Control flow

The usual Pascal control structures are all here. A block of more than one
statement is wrapped in `begin ... end`.

```pascal
program flow;
var i, n: integer;
begin
  n := 0;
  for i := 1 to 10 do
  begin
    if (i mod 2) = 0 then
      n := n + i;          { sum the even numbers }
  end;
  WriteLn('sum of evens 1..10 = ', n);

  i := 1;
  while i <= 3 do
  begin
    WriteLn('while ', i);
    i := i + 1;
  end;

  repeat
    WriteLn('repeat ', i);
    i := i - 1;
  until i = 0;
end.
```

`and` and `or` short-circuit, so a left operand can guard the right:
`(i <= n) and (a[i] = x)` is safe when `i` might be out of range. `break` and
`continue` work inside any loop.

## 4. Procedures and functions

A procedure returns nothing; a function returns a value, assigned to the
function's own name inside the body. Routines may be nested and may recurse.

```pascal
program fib;
var i: integer;

function fib(n: integer): integer;
begin
  if n < 2 then
    fib := n
  else
    fib := fib(n - 1) + fib(n - 2);
end;

begin
  for i := 0 to 10 do
    WriteLn('fib(', i, ') = ', fib(i));
end.
```

Parameters are by value unless marked `var` (by reference, so the routine can
change the caller's variable) or `const` (by reference but read-only, used for
large values like arrays and records you do not want copied).

## 5. Arrays and records

An array has a fixed index range; a record groups named fields. Both are value
types: assigning one copies it.

```pascal
program shapes;
type
  Vec   = array[1..3] of integer;
  Point = record x, y: integer; end;
  Line  = record a, b: Point; len: real; end;
var
  v: Vec;
  seg: Line;
  i: integer;

function dot(const u, w: Vec): integer;
var acc, k: integer;
begin
  acc := 0;
  for k := 1 to 3 do
    acc := acc + u[k] * w[k];
  dot := acc;
end;

begin
  for i := 1 to 3 do
    v[i] := i;
  WriteLn('dot(v, v) = ', dot(v, v));

  seg.a.x := 10;
  seg.a.y := 20;
  seg.b := seg.a;      { copies the whole record }
  seg.len := 2.5;
  WriteLn('seg.b.x = ', seg.b.x);
end.
```

Arrays can be multi-dimensional (`array[0..3, 1..2] of integer`), and a field
or element can itself be an array or record.

## 6. Enumerations and sets

An enumerated type lists its values; a set holds a subset of an ordinal type.

```pascal
program colors;
type
  Direction = (north, east, south, west);
  DirSet    = set of Direction;
var
  d: Direction;
  exits: DirSet;
begin
  d := east;
  exits := [north, east];
  if d in exits then
    WriteLn('can go east');
  if not (south in exits) then
    WriteLn('south is blocked');
end.
```

## 7. Strings and your own output

`string` is a host-managed value type, not a block of memory. You can build
output from mixed arguments, and you can add your own overload of an output
routine for a type of your own.

```pascal
program greet;
var name: string;

procedure greet(const who: string);
begin
  WriteLn('Hello, ', who, '!');
end;

begin
  name := 'world';
  greet(name);
end.
```

## 8. Host handles and methods

A game or tool embeds Kobold and exposes its own objects to scripts as opaque
handles. A script declares the handle type with `interface` syntax and calls
methods on it; the host dispatches the call. The script never sees the object's
memory, only the handle.

```pascal
program move;
type
  Entity = interface
    move: procedure (dx, dy: integer);
    name: function : string;
  end;

procedure step(e: Entity);
begin
  e.move(1, 0);
  WriteLn(e.name, ' moved');
end;

var guard: Entity;
begin
  guard := spawnGuard;   { a host function that returns a handle }
  step(guard);
end.
```

This program needs a host that registers `Entity` and provides `spawnGuard`, so
it is compiled against a host rather than run bare. See `kp-embedding.md` for
how the host side is written, and `kp-abi.md` for the handle contract.

## 9. Coroutines

A coroutine runs until it reaches a `yield`, then suspends. `start` creates one
and gives back a `process` handle; you resume it by calling that handle like a
function, `k(v)`. The first call runs the body from the top to its first
`yield`, and each later call continues from where it suspended. `yield E` hands
`E` back as the value of that call. This is how a script expresses behavior that
unfolds over many frames.

```pascal
program ticker;
var
  k: process;
  i: integer;

procedure counter;
var n: integer;
begin
  for n := 1 to 3 do
    yield n;           { suspend, handing n back to the resumer }
end;

begin
  k := start counter;  { create the coroutine; the body has not run yet }
  for i := 1 to 3 do
    WriteLn('tick ', k(0));   { each call runs to the next yield }
end.
```

A coroutine keeps its own stack, so it can be suspended at any call depth and
resumed later. The argument you pass in a resume, `k(0)` above, becomes the
value of the `yield` that was waiting. `release(k)` cancels one you will not
resume. See the Coroutines section of `kp-guide.md` for `bind` and the
`process` value.

## 10. Splitting a program into units

A larger program is divided into units that compile separately and are linked by
`uses`. A unit has an interface part (what it exports) and an implementation
part (the bodies). See `kp-units.md` for the full model, including interface
fingerprints and guest-record traits.

## 11. Inline assembly

When you need an instruction the language does not expose, `asm('text')` drops
one line straight into the generated assembly:

```
program spin;
var
    i: integer;
begin
    for i := 1 to 3 do begin
        asm('/* unrolled spin tick */');   { emitted verbatim }
        WriteLn('tick ', i);
    end;
end.
```

The string is passed through unchanged, so its text must match the assembler
for the target you are building. Here the payload is a C-style comment, which
every target assembler accepts, so the program builds on each backend. For a
real instruction you would write target-specific text, for example
`asm('syscall')` on x86-64 or `asm('svc #0')` on AArch64.

In this basic form there are no operand constraints and no register
substitution: the compiler does not read the text and register allocation does
not see it, so the block must leave the machine state the surrounding code
expects. The literal is taken as written, with no `${...}` interpolation even in
a double-quoted string.

To hand values to the template, use the extended form, which adds GNU-style
operand sections. Each operand is a constraint string and a parenthesised value,
and `%N` in the template is operand N (outputs first, then inputs):

```
asm('add %0, %1' : '+r'(a) : 'r'(b));   { a := a + b, both in registers }
```

The supported subset is integer-class operands and the general register class:
`=r` for an output, `+r` for a read-write output, and `r` for an input, with a
final section listing any clobbered registers. The compiler places each operand
in a register and substitutes its name into the template.

Inline assembly is ahead-of-time only. The in-process JIT emits machine code
directly and has no assembler for a verbatim string, so a program that uses
`asm` must be compiled with the AOT path (`skj-kp`); running it under the JIT
reports that inline asm is unsupported. See `kp-grammar.md` for the grammar and
`kp-language.md` for the reference.

## Where to go next

- `kp-examples.md` collects complete, annotated programs to compile and run.
- `kp-language.md` is the full reference; `kp-grammar.md` is the grammar.
- `kp-errors.md` explains failable routines and run-time traps.
- `kp-guide.md` covers embedding and the ahead-of-time build.
