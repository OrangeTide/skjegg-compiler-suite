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

## Records cross by reference

A `record` crosses the boundary of an `external` routine by reference, as a C
pointer: a `var` record parameter is a C `T*`, a `const` record parameter a
`const T*`. The glue reads and writes the struct's fields through that pointer, and
a `var` record's changes are visible to the caller on return. A record's field
layout matches the equivalent C struct for plain data: fields sit at their natural
offsets with the same padding, and a `single` field is a C `float`. So a C struct
is bound by declaring a matching record and passing it `var` or `const`.

```pascal
type Rect = record x: integer; y: integer; w: integer; h: integer; end;

{ C: void fill_rect(const struct Rect *r);  reads the four fields }
procedure DrawRect(const r: Rect); cdecl; external name 'fill_rect';

{ C: void get_size(int id, int *w, int *h);  writes through two pointers }
procedure GetSize(id: integer; var w: integer; var h: integer); cdecl; external;
```

This replaces decomposing a struct into separate scalar arguments or reading it back
through one accessor per field: the glue takes a pointer to the record and works on
it directly.

A record that crosses the FFI must be plain data. Its fields may be scalars, nested
records, and fixed arrays of those, in any combination (a variant record works too,
overlaying like a C union). A field that is a `string`, dynamic array, `map`,
`interface`, or process, or one nested inside a field, is a reference-counted
handle rather than inline bytes, so the record would not have C struct layout; such a
field is rejected at compile time, naming the field. Pass that data separately (a
`string` argument crosses on its own as a handle, see below) or keep it out of the
struct.

## Strings cross as a handle

A `string` argument does not cross as a C `char *`. A Kobold string is a managed
guest-heap object, and what reaches the glue is its handle (a `kp_gptr`), not the
address of its bytes. The glue turns the handle into a NUL-terminated C string with
`kp_str_bytes`, the read side the runtime exposes in `kobold/str.h`:

```c
#include "str.h"      /* kp_gptr, kp_str_bytes */

/* Kobold: function LoadImage(const path: string): integer; cdecl; external; */
int
kpimg_load(kp_gptr path)
{
    const char *cpath = kp_str_bytes(path);   /* NUL-terminated; "" for nil */
    return decode(cpath);
}
```

The string runtime resolves from `libkpaot.a`, whose no-vm signature the glue matches
by compiling with `-DKP_FREESTANDING` (and `-Ikobold -Iir` for the headers). A glue
function that declared the parameter as `const char *` would instead receive the raw
handle and read the object header as if it were text, so always take a `string`
argument as `kp_gptr` and call `kp_str_bytes`. To hand a C string back to Kobold,
build a new string with `kp_str_new(bytes, len)` and return the `kp_gptr`.

`kp_gptr` is a guest pointer, so it is pointer-width: compile the glue with
`-DSKJ_LP64=1` on the 64-bit targets (x86-64 and arm64), so it is 64 bits and matches
the runtime. Without that the kobold headers default it to 32 bits, and a guest
pointer passed to the glue is truncated whenever the guest heap happens to map above
4 GB (which a unit that pulls in large shared libraries can cause), a crash that hides
until the memory layout shifts.

### Field widths

An integer field is 4 bytes, matching a C `int`. The narrow integer types have
their natural storage width, so each maps to the C type of that width: `byte` and
`shortint` are 1 byte (C `unsigned char` and `signed char`), `word` and `smallint`
are 2 bytes (C `unsigned short` and `short`). `cardinal` and `longint` are the
4-byte integer. An `int64` is 8 bytes, a `single` 4, a `real` 8. A narrow field
sits at its own natural alignment (a `word` on a 2-byte boundary) and is loaded and
stored at its own width, zero-extended for the unsigned types and sign-extended for
the signed ones, so a record of narrow fields has the same layout as the C struct
of the matching `char`/`short` fields. `SDL_Color` (four `Uint8`) is
`record r, g, b, a: byte; end`.

### Packing

A record's fields are laid out at their natural alignment by default, the same as a
C struct. To match a C struct declared `#pragma pack(n)` or
`__attribute__((packed))`, control the alignment:

- `packed record ... end` removes the padding between fields (field alignment 1), as
  a packed C struct does.
- `{$A n}` caps field alignment at *n* (1, 2, 4, 8, or 16) for the records that
  follow it; `{$A1}` is packed, `{$A+}` restores natural alignment.

Packing changes only the padding between fields, so it matters when a record mixes
widths. A packed `record a: integer; b: int64; end` places `b` at offset 4, where a
natural record pads it to offset 8, matching a C `__attribute__((packed))` struct of
an `int` and a `long long`.

### By value

A small plain-data record crosses a `cdecl` boundary **by value** on the 64-bit targets
and on `rv32`, passed in registers and returned in them, following the C register ABI. The
record's fields are plain data: `byte` through `int64`, `single`, `real`, and nested
records or fixed arrays of those, but no managed field (a `string`, array, map, interface,
or process). The ABI places it:

- **x86-64 (SysV):** a record of 16 bytes or less rides one or two eightbytes. An eightbyte
  made only of floats is an SSE class, passed in an `xmm` register; any eightbyte with an
  integer byte is the INTEGER class, passed in a general register.
- **arm64 (AAPCS64):** a Homogeneous Floating-point Aggregate, one to four members all
  `single` or all `real`, rides the `s` or `d` registers; any other record of 16 bytes or
  less rides one or two general registers.
- **rv32 (ilp32):** a record of 8 bytes or less rides one or two 4-byte integer words. The
  soft-float ABI has no floating-point register rule, so a float field rides an integer
  word like any other.

So a C function that takes or returns such a record by value binds with a plain by-value
parameter or result:

```pascal
type Color = record r, g, b, a: byte; end;      { 4 bytes, one integer register }
type FColor = record r, g, b, a: single; end;    { 16 bytes: SSE eightbytes / a float HFA }
function Luma(c: Color): integer; cdecl; external;        { passed by value }
function Blend(a: FColor; b: FColor): FColor; cdecl; external;   { floats, by value }
```

When a record's register slots do not all fit in the argument registers still available at
the call, the whole record is passed on the stack instead, as the psABI requires.

Everything outside the register classes still crosses by reference, and a by-value use is
rejected at compile time: a record too large for the register class (over 16 bytes on the
64-bit targets, over 8 bytes on `rv32`), a record with a managed field, and the ColdFire
and MIPS targets, whose ABI has no register struct class. Bind those with a glue wrapper
that takes a pointer, or return the value through a `var` parameter.

A Kobold `cdecl` routine is both sides of this. As a caller it passes and returns a record
by value to a C function, as above. As a callee, when C calls the routine back, it receives
a by-value record in the same registers and reconstructs it, and scatters a record result
into the return registers. So a Kobold `cdecl` callback may take or return a small plain-data
record by value, and C calls it exactly as it would a C function with that signature. The
same rejections apply to the callback's own signature: a by-value record it takes must be a
register-class plain-data record, or the parameter must be `var` / `const`.

The by-value record return also works through an **indirect** call, a call through a `cdecl`
procedure value (a C function pointer) rather than a routine named at the call site. A C
function pointer that returns a small record by value, held in a `cdecl` proc-typed variable
or field, returns the record in registers just as a direct call does, so the result binds
with no wrapper.

## Callbacks: passing a Kobold routine to C

A Kobold procedure value is a bare code pointer emitted through the platform C ABI,
so a native-backed unit may hand a routine to its C library as a function pointer and
the library may call it back. Declare the callback type `cdecl` and pass the routine
with `@name` (see the "Procedure types" section of `kp-language.md`). `std.osthreads`
is built on exactly this: the thread body is a Kobold routine the glue runs on a real
OS thread.

A callback declared `cdecl` may take or return a small plain-data record by value, not
only scalars: the routine reconstructs a by-value record parameter from the argument
registers and returns a record result in the return registers, so a C library that calls
back with a struct by value binds directly.

```pascal
type FPoint = record x, y: single; end;        { 8 bytes: an SSE eightbyte / a float HFA }
type MapFn = function(p: FPoint): single cdecl; { the C callback type }

{ the Kobold callback C calls back with an FPoint by value }
function Length2(p: FPoint): single; cdecl;
begin Length2 := p.x * p.x + p.y * p.y; end;
```

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

## Unit routine names do not collide with the C library

A unit compiles its public routines to symbols qualified by the unit name,
`<unit>.<name>`, not the bare name. So a unit may give its routines the same names
as the C library it binds without the two colliding at link time. The `raylib`
unit binds raylib's own `InitWindow`, `DrawRectangle`, and `DrawText`: the unit's
wrappers are emitted as `raylib.InitWindow` and so on, while the glue's calls to
the C `InitWindow` resolve to raylib. A consumer that `uses raylib` calls
`rl.InitWindow`, which the compiler resolves to the `raylib.InitWindow` symbol.

Only a unit's own routines are qualified. A program's routines keep their bare
names, a method is qualified by its receiver type, and an `external` or `cdecl`
routine keeps the exact C symbol it binds, so the glue entry points a unit imports
(`kpray_init_window`) are never renamed. A routine re-exported from a dependency
keeps that dependency's qualifier, so it names the same symbol everywhere.

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

## Using the sdl3 unit

`sdl3` binds SDL3 for window creation, 2D rendering, and polled input. It builds
only when `./configure` found SDL3 (`HAVE_SDL3`), so it is the worked example of an
optional unit gated on an external dependency. It is the record-binding counterpart
to `std.osthreads`: the SDL structs are Kobold records that cross by reference, so
the API reads like the C one.

`Color` is `record r, g, b, a: byte; end`, SDL_Color's exact four-byte layout; `Rect`
and `FRect` are the integer and float rectangles; `Event` is a 128-byte buffer
`PollEvent` fills in place. Windows and renderers are integer ids (the glue holds the
real pointers). SDL entry points that take a struct by value, like the color setter's
four `Uint8` arguments, are bridged in the glue, which reads the record's fields.

```pascal
program fill;
uses sdl3 as sdl;
var win, ren: integer; col: sdl.Color; box: sdl.FRect; ev: sdl.Event;
begin
  if not sdl.InitVideo then halt(1);
  win := sdl.CreateWindow('fill', 320, 240);     { an integer window id }
  ren := sdl.CreateRenderer(win);
  col.r := 10; col.g := 20; col.b := 30; col.a := 255;   { byte fields }
  box.x := 10.0; box.y := 10.0; box.w := 100.0; box.h := 80.0;  { single fields }
  sdl.SetDrawColor(ren, col);                    { Color passed by reference }
  sdl.Clear(ren);
  sdl.FillRect(ren, box);                        { FRect passed by reference }
  sdl.Present(ren);
  while sdl.PollEvent(ev) do                      { PollEvent fills the buffer }
    if sdl.EventType(ev) = sdl.EvQuit then halt(0);
  sdl.DestroyRenderer(ren); sdl.DestroyWindow(win); sdl.Quit;
end.
```

`make check-kp-sdl3` runs this surface headless under the dummy video driver and the
software renderer, so it needs no display.

## Using the stb_image unit

`stb_image` binds stb_image.h, the public-domain single-header image loader (PNG,
JPEG, BMP, TGA, GIF, and more). The header is vendored at `third_party/stb_image.h`,
so unlike `sdl3` the unit has no external dependency and is always built. It is the
worked example of the string-handle and integer-id patterns together: a path crosses
as a `string`, and a decoded image is named by an integer id, since stb returns the
pixels as a pointer Kobold cannot hold.

`Load` decodes a file and returns an id (> 0, or 0 on failure); `LoadRGBA` forces four
channels. `Width`, `Height`, and `Channels` report the image, and `Pixel(img, x, y,
channel)` reads one channel byte (0..255), with `y = 0` the top row. `Free` releases
the image.

```pascal
program thumb;
uses stb_image as img;
var h, x, y: integer;
begin
  h := img.Load('sprite.png');               { a string path; 0 on failure }
  if h = 0 then halt(1);
  writeln(img.Width(h), 'x', img.Height(h), ' ', img.Channels(h), ' channels');
  for y := 0 to img.Height(h) - 1 do
    for x := 0 to img.Width(h) - 1 do
      { img.Pixel(h, x, y, 0) is red, 1 green, 2 blue, 3 alpha }
      ;
  img.Free(h);
end.
```

`make check-kp-stb-image` decodes a committed 3x2 BMP whose pixels are known and
checks the dimensions and every channel.

## Using the stb_truetype unit

`stb_truetype` binds stb_truetype.h, the public-domain TrueType font loader and
rasterizer, vendored at `third_party/stb_truetype.h`. Like `stb_image` it is
header-only, so it has no external dependency and is always built. It uses two
integer-id handles: `LoadFont` returns a font id (the glue keeps the file bytes
alive, since stb reads glyph outlines straight from them), and `Render` rasterizes
one code point at a pixel height into a glyph id, an 8-bit coverage bitmap.

`Ascent`, `Descent`, and `LineGap` give the vertical metrics scaled to a pixel
height; `Advance` gives a code point's horizontal advance. A rendered glyph reports
`GlyphWidth`, `GlyphHeight`, and the top-left offset `GlyphXOff` / `GlyphYOff` from
the pen origin, and `Coverage(glyph, x, y)` reads one anti-aliased pixel (0..255).

```pascal
program caption;
uses stb_truetype as ttf;
var font, glyph, x, y: integer;
begin
  font := ttf.LoadFont('font.ttf');            { a font id; 0 on failure }
  if font = 0 then halt(1);
  glyph := ttf.Render(font, 65, 32);           { 'A' at 32 px -> a glyph id }
  for y := 0 to ttf.GlyphHeight(glyph) - 1 do
    for x := 0 to ttf.GlyphWidth(glyph) - 1 do
      { ttf.Coverage(glyph, x, y) is the pixel's alpha, 0..255 }
      ;
  ttf.FreeGlyph(glyph);
  ttf.FreeFont(font);
end.
```

`make check-kp-stb-truetype` loads a vendored OFL font
(`third_party/fonts/SourceCodePro-Regular.ttf`) and checks the metrics and a
rasterized glyph. The font and stb are version-pinned, so the numbers are identical
on x86-64 and arm64.

## Using the netchan unit

`netchan` binds netchan, a multiplexed reliable/unreliable channel protocol, over
UDP. It is the first unit over a vendored multi-file library rather than a single
header: the netchan core and its UDP transport are vendored at
`third_party/netchan/`, a small CC0 library built to be copied in this way, so the
unit has no external dependency and is always built.

netchan is transport-agnostic, so the glue owns the UDP socket and the clock. A
connection and a channel are each an integer id. `OpenServer` and `OpenClient`
create a connection; `Service` runs the socket pump and returns the connection
state, so a program drives the link by calling it in a loop. The sender opens a
channel with `OpenChannel` and `Send`s datagrams; the receiver learns of delivered
data from `Poll` (an `EvData` event) and takes it with `Recv`.

```pascal
program echo;
uses netchan as nc;
var conn, ch, ev: integer;
begin
  conn := nc.OpenClient('127.0.0.1', 27015);
  while nc.Service(conn) <> nc.StateConnected do ;   { pump until connected }
  ch := nc.OpenChannel(conn, nc.Reliable, nc.DirSend, 'text');
  nc.Send(ch, 'hello');
  repeat
    nc.Service(conn);
    ev := nc.Poll(conn);
    if ev = nc.EvData then writeln(nc.Recv(conn));
  until ev = nc.EvDisconnected;
  nc.Close(conn);
end.
```

`make check-kp-netchan` runs a server and a client in one process over loopback
UDP, exchanges a reliable datagram, and checks it arrives, so it needs no external
network.

## Using the curl unit

`curl` binds libcurl's easy interface for a simple fetch. It is built only when
`./configure` found libcurl (`HAVE_LIBCURL`), so it is a dependency-gated unit like
`sdl3`. `Get` runs a whole transfer and returns the body; `LastStatus` and `LastOk`
report the outcome of the last one. libcurl delivers the body in chunks through a
write callback, which the glue supplies and accumulates into the returned string.

Any scheme libcurl supports works, so a `file://` URL reads a local file through the
same path as an `http://` fetch.

```pascal
program fetch;
uses curl as http;
var page: string;
begin
  page := http.Get('https://example.com/');
  if http.LastOk and (http.LastStatus = 200) then
    writeln(page)
  else
    writeln('fetch failed, status ', http.LastStatus);
end.
```

`make check-kp-curl` fetches a committed fixture through a `file://` URL, so it needs
no network.

## Using the raylib unit

`raylib` binds raylib for a window, immediate-mode 2D drawing, and a few color and
vector helpers. It is built only when `./configure` found raylib (`HAVE_RAYLIB`), so
it is a dependency-gated unit like `sdl3`. raylib often installs as a static library
with no `pkg-config` file, so configure probes the header and the static link line
directly when `pkg-config` has no `raylib`.

It is the worked example of the **by-value record FFI**: raylib passes `Color`,
`Vector2`, and `Rectangle` by value in registers and returns some by value, so the
unit's records cross by value rather than by reference. `Color` is four `byte`
fields with raylib's layout; `Vector2` and `Rectangle` are `single` floats. The unit
keeps raylib's own routine names, which the unit namespacing keeps from colliding
with raylib's C symbols.

```pascal
program window;
uses raylib as rl;
var c: rl.Color;
begin
  c.r := 30; c.g := 120; c.b := 200; c.a := 255;
  rl.InitWindow(640, 480, 'hello');
  while not rl.WindowShouldClose do begin
    rl.BeginDrawing;
    rl.ClearBackground(rl.GetColor(0));
    rl.DrawRectangle(20, 20, 100, 60, c);
    rl.DrawText('hello', 40, 40, 20, c);
    rl.EndDrawing;
  end;
  rl.CloseWindow;
end.
```

raylib needs a real GL context, so unlike `sdl3` there is no dummy driver.
`make check-kp-raylib` opens the window and draws one frame under a virtual X server
(`xvfb-run`) with software GL, so it is CI-safe without a GPU.

See `kp-units.md` for the unit model in general and `kobold.md` for the ahead-of-time
build.
