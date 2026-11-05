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

The `skjcc` driver has a `ps1` target (alias `psx`) that runs the whole pipeline
and writes a PS-EXE. It dispatches on the source extension, so the same command
builds C, Pascal, or Kobold:

```sh
skjcc -target ps1 -o cube.exe cube.c      # C
skjcc -target ps1 -o cube.exe cube.pas    # Pascal
skjcc -target ps1 -o cube.exe cube.kp     # Kobold
```

Each form runs the stages that language needs and links the crt and `libpsx.a`
for you. C compiles with the soft-float MIPS compiler and finds `psx.h` through
the installed SDK. Pascal compiles straight to assembly; the `cube.pas` sample
declares the `libpsx` bindings inline (it copies the block from the installed
`psx.pas`), so there is no unit to link. Kobold runs its two stages (the front
end emits a KPM module, the codegen lowers it to assembly), resolves `uses psx`
from the installed unit directory, and links the prebuilt `psx_unit.o` binding.
The driver keeps the `.s` and `.kpm` intermediates in a temporary directory and
removes them; pass `-save-temps` to keep them, or `-S` to stop at assembly.

Under the driver, the three raw pipelines look like this. You do not need to run
them by hand, but they show what each `skjcc` call does:

```sh
LIB="$HOME/.local/lib/skjegg/ps1"

# Pascal: front end to assembly, assemble, link.
skj-pc-mips -o cube.s cube.pas
skj-as-mips -o cube.o cube.s
skj-ld-mips -f ps-exe -o cube.exe "$LIB/start_psx.o" cube.o "$LIB/libpsx.a"

# Kobold: front end to KPM, codegen to assembly, assemble, link the unit.
skj-kpc-mips -U "$LIB" --emit-kpm cube.kp > cube.kpm
skj-kp-mips-sf cube.kpm -o cube.s
skj-as-mips -o cube.o cube.s
skj-ld-mips -f ps-exe -o cube.exe \
    "$LIB/start_psx.o" cube.o "$LIB/psx_unit.o" "$LIB/libpsx.a"
```

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
`PREFIX` to wherever you installed skjegg. Each language builds with one `skjcc`
call; keep whichever rule matches your source and delete the rest.

```make
# A libpsx game project. Expects skjegg installed under $(PREFIX).
PREFIX ?= $(HOME)/.local
BIN    := $(PREFIX)/bin

BIOS   ?= $(HOME)/.pcsx/bios/bios.bin
REDUX  ?= pcsx-redux

# One command per language; the driver picks the pipeline from the extension.
game.exe: game.c
	$(BIN)/skjcc -target ps1 -o $@ $<

# For a Pascal game, swap the prerequisite and source:
#   game.exe: game.pas
#       $(BIN)/skjcc -target ps1 -o $@ $<
# For a Kobold game (its `uses psx` resolves from the installed SDK):
#   game.exe: game.kp
#       $(BIN)/skjcc -target ps1 -o $@ $<

# Run the build in the emulator.
run: game.exe
	$(REDUX) -bios $(BIOS) -exe game.exe

clean:
	rm -f *.exe

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
