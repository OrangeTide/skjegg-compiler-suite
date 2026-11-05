# Setting up the PlayStation toolchain

This is a step-by-step guide to getting a working PlayStation (original PS1,
R3051) cross-compiler toolchain from skjegg, building the spinning-cube sample
in C, Pascal, and Kobold, and running it. For the SDK itself (the drivers, the
memory budget, the asset path) see [the PlayStation SDK](psx-sdk.md) and [the API
reference](psx-api.md).

skjegg is binutils-free for MIPS, so no external cross toolchain is needed: the
assembler, linker, and archiver are in-tree. You only need a host C compiler to
build skjegg, and a PlayStation emulator plus a BIOS image to run what you make.

## Prerequisites

- A host C compiler (`cc`, C99) to build skjegg.
- For running the result: a PlayStation emulator that loads a PS-EXE, and a PS1
  BIOS image. This guide uses PCSX-Redux (what the project tests against), but
  any emulator that loads a PS-EXE works, as does real hardware.

## 1. Build and install

From a checkout, build everything and install:

```sh
make
make install PREFIX="$HOME/.local"
```

`make install` stages the tools into `$(PREFIX)/bin` and the runtime into
`$(PREFIX)/lib/skjegg`. `PREFIX` defaults to `~/.local`; `DESTDIR` prepends a
staging root (for packaging). Put the tools on your `PATH`:

```sh
export PATH="$HOME/.local/bin:$PATH"
```

The install places, for the PlayStation:

- **Tools in `$(PREFIX)/bin`:** `skjcc` (the cross driver), the MIPS C compiler
  `skj-cc-mips-sf`, the Pascal and Kobold front ends `skj-pc-mips`,
  `skj-kpc-mips`, `skj-kp-mips-sf`, the assembler and linker `skj-as-mips` and
  `skj-ld-mips`, the archiver `skj-ar`, the romfs builder `skj-mkromfs`, and the
  asset converters `skj-timconv` (PPM to TIM), `skj-vagconv` (PCM to SPU-ADPCM),
  and `skj-mkcdda` (a CD-DA test disc).
- **SDK in `$(PREFIX)/lib/skjegg/ps1`:** `libpsx.a`, the `start_psx.o` crt,
  `psx.h`, the `psx.pas` and `psx.kp` units, and the prebuilt Kobold unit module
  `psx.kpm` with its object `psx_unit.o`.

The driver finds that SDK directory relative to its own location, so once `bin`
is on `PATH` nothing else needs setting.

## 2. Build the cube

The sample sources are in `examples/psx/` (`cube.c`, `cube.pas`, `cube.kp`). Copy
the one you want into your project and build it.

### C: through the driver

The `skjcc` driver has a `ps1` target (alias `psx`) that runs the whole C
pipeline (compile with the soft-float MIPS compiler, assemble, link the crt and
`libpsx.a`, and write a PS-EXE), and finds `psx.h` and the SDK for you:

```sh
skjcc -target ps1 -o cube.exe cube.c
```

That one command is the C equivalent of the three-step Pascal and Kobold builds
below.

### Pascal: the front end and the in-tree linker

The Pascal front end targets MIPS directly. The `cube.pas` sample declares the
`libpsx` bindings inline (it copies the block from the installed `psx.pas`), so
there is no unit to link, just the program, the crt, and `libpsx.a`:

```sh
LIB="$HOME/.local/lib/skjegg/ps1"
skj-pc-mips -o cube.s cube.pas
skj-as-mips -o cube.o cube.s
skj-ld-mips -f ps-exe -o cube.exe "$LIB/start_psx.o" cube.o "$LIB/libpsx.a"
```

### Kobold: the two-stage front end and the psx unit

Kobold compiles in two steps (the front end emits a KPM module, then the
codegen emits assembly). The `cube.kp` sample does `uses psx`, so point the unit
search at the installed SDK directory (which holds `psx.kpm`) and link the
prebuilt `psx_unit.o`:

```sh
LIB="$HOME/.local/lib/skjegg/ps1"
skj-kpc-mips -U "$LIB" --emit-kpm cube.kp > cube.kpm
skj-kp-mips-sf cube.kpm -o cube.s
skj-as-mips -o cube.o cube.s
skj-ld-mips -f ps-exe -o cube.exe \
    "$LIB/start_psx.o" cube.o "$LIB/psx_unit.o" "$LIB/libpsx.a"
```

(A single driver target for Pascal and Kobold, so these also build with one
`skjcc` command, is planned. For now the C path uses the driver and these two
use the tools directly.)

## 3. Run it

Each build above produces `cube.exe`, a standard PS-EXE. Load it in an emulator
with a BIOS. With PCSX-Redux:

```sh
# windowed
pcsx-redux -bios /path/to/bios.bin -exe cube.exe

# headless (software renderer, no window), the way the project tests run
pcsx-redux -cli -softgpu -testmode -run -no-gui-log \
    -bios /path/to/bios.bin -exe cube.exe
```

You should see a shaded cube spinning on a dark field. Any emulator that loads a
PS-EXE will run it, and so will real hardware through the usual loaders.

## 4. An example project Makefile

A self-contained Makefile for a project that keeps its sources beside it. Set
`PREFIX` to wherever you installed skjegg. It builds the C program through the
driver and includes the Pascal and Kobold rules for reference; delete the ones
you do not use.

```make
# A libpsx game project. Expects skjegg installed under $(PREFIX).
PREFIX ?= $(HOME)/.local
BIN    := $(PREFIX)/bin
LIB    := $(PREFIX)/lib/skjegg/ps1

BIOS   ?= $(HOME)/.pcsx/bios/bios.bin
REDUX  ?= pcsx-redux

# --- C: one command through the driver ---
game.exe: game.c
	$(BIN)/skjcc -target ps1 -o $@ $<

# --- Pascal: front end, assemble, link (bindings declared inline) ---
game_pas.exe: game.pas
	$(BIN)/skj-pc-mips -o game_pas.s $<
	$(BIN)/skj-as-mips -o game_pas.o game_pas.s
	$(BIN)/skj-ld-mips -f ps-exe -o $@ \
	    $(LIB)/start_psx.o game_pas.o $(LIB)/libpsx.a

# --- Kobold: emit KPM, codegen, assemble, link the psx unit ---
game_kp.exe: game.kp
	$(BIN)/skj-kpc-mips -U $(LIB) --emit-kpm $< > game_kp.kpm
	$(BIN)/skj-kp-mips-sf game_kp.kpm -o game_kp.s
	$(BIN)/skj-as-mips -o game_kp.o game_kp.s
	$(BIN)/skj-ld-mips -f ps-exe -o $@ \
	    $(LIB)/start_psx.o game_kp.o $(LIB)/psx_unit.o $(LIB)/libpsx.a

# Run the C build in the emulator.
run: game.exe
	$(REDUX) -bios $(BIOS) -exe game.exe

clean:
	rm -f *.exe *.o *.s *.kpm

.PHONY: run clean
```

Assets, if the game has them, go through the installed converters:
`skj-timconv` turns a PPM into a TIM texture, `skj-vagconv` turns PCM into an
SPU-ADPCM sample, and `skj-mkromfs` packs a directory into a romfs image to embed
or load. See [the PlayStation SDK](psx-sdk.md) for the asset path.

## Building in the tree instead of installing

If you are working inside the skjegg checkout and would rather not install, the
Makefile builds the same samples under `build/psx/`:

```sh
make build/psx/cube.exe        # C
make build/psx/cube_pas.exe    # Pascal
make build/psx/cube_kp.exe     # Kobold
```

These reference the in-tree SDK directly (`build/mips/libpsx.a`,
`build/mips/start_psx.o`, and the unit under `build/psx-units/`). To build and
run the cube under PCSX-Redux with the framebuffer self-check, with a BIOS
configured, run `make test-psx-cube-redux`. Your own project can link against the
in-tree artifacts the same way the installed layout does, substituting
`build/mips` and `build/psx-units` for `$(LIB)`.
