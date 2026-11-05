# The PlayStation SDK

A shared runtime SDK that lets C, Compact Pascal, and Kobold programs drive the
original PlayStation (SCPH-100x, the R3051 at 33.8 MHz) well enough to make
games: its GPU, SPU, controllers, timers, and CD-ROM. The SDK is one C and
assembly library, `libpsx`. Pascal and Kobold reach it through thin units that
bind to the C names and add no logic of their own.

This document is the plan. It assumes the MIPS toolchain described in
`doc/tools/mips.md`, which already reaches the console: the MIPS I backend, the
soft-float configuration for the FPU-less R3051, the binutils-free toolchain
(`skj-as-mips`, `skj-ld-mips`, `skj-ar`), the PS-EXE output mode
(`skj-ld-mips -f ps-exe`), and the BIOS-TTY runtime `runtime/start_psx.S`. That
work is done. This plan is the layer above it.

## Scope

The target is 2D games first, and the work is prototyped in C. C is the surface
the whole SDK is proven on: the drivers, the asset path, and a complete sample
game all come up in C before either other language is involved. Pascal and
Kobold then lean on that C layer heavily, through thin units, rather than
reimplementing any of it. This orders the plan so the `libpsx` API is exercised
and stable before a single unit is written against it.

2D comes before 3D, but the gap is smaller than it looks. The PlayStation GPU
draws screen-space primitives, triangles and quads with texture coordinates,
and those same primitive packets are what a 3D scene feeds the GPU too. The 2D
path builds and ships exactly that packet stream. 3D adds a front end that
produces the vertices: the Geometry Transformation Engine (the GTE, coprocessor
2) transforms and projects model vertices into the screen-space quads the 2D
primitive path already draws. So 3D is largely the GTE plus a vertex feed on top
of a 2D primitive pipeline that already exists, not a separate renderer. The MIPS
backend does not emit COP2 instructions, so the GTE is reached through a small
assembly helper library (`libpsx/gte.S`) rather than new backend code (milestone
8). Milestones 1 through 7 are 2D: sprites, backgrounds, and flat or textured
triangles and quads shipped to the GPU by DMA.

The platform target is a retail PlayStation booting its own BIOS, the same
target the PS-EXE runtime already serves. Outside-world entry points route
through the BIOS A0/B0/C0 call tables where a BIOS routine exists, and through
direct memory-mapped I/O where speed or control demands it. This is the
portable homebrew convention and matches the BIOS-TTY choice already made for
`write` and `exit`.

The order of work follows from this. The C graphics bring-up comes first
(milestones 1 through 4), so the API the units bind to is settled by running
real C against it. The Pascal and Kobold front-end ports and their units come
after (milestones 5 and 6), binding to an API that is already proven. The
earlier draft of this plan put the front-end ports first; prototyping in C and
porting the languages afterward is the deliberate reversal.

## The shared-SDK architecture

```
  C game      Pascal game         Kobold game
    │            │ uses psx          │ uses psx
    │      ┌─────┴────────┬──────────┘
    │      │  psx.pas     │  psx.kp        thin units: records and external
    │      │  unit        │  unit          declarations, no logic
    │      └─────┬────────┴──────────┐
    └────────────┴───────────────────┘
                 │  one C ABI (the toolkit stack convention,
                 │   which all three front ends already emit)
          ┌──────┴───────────────────────────────────────┐
          │  libpsx   (C compiled soft-float, plus asm)   │
          │  gpu.c  dma.c  irq.c  spu.c  pad.c  timer.c    │
          │  cd.c   fixed.c   fs glue   asm BIOS shims     │
          └──────┬───────────────────────────────────────┘
                 │  MMIO (volatile load/store) + BIOS jumps
          start_psx.S   +   skj-ld-mips -f ps-exe
```

Two decisions hold the design together.

**One calling convention.** TinC, TinScheme, the default C compiler, and the
Kobold AOT path all already emit the toolkit's own stack-passing convention
(arguments pushed right to left, caller pops, result in `$v0`; see "The calling
convention" in `doc/tools/mips.md`). So a single `libpsx.a` built with
`skj-cc-mips-sf` is callable from all three front ends with no bridging. The
only code that needs the console's o32 and BIOS register conventions is the
handful of assembly shims that jump into the BIOS, and those bridge the two
conventions the way `start_psx.S`'s `write` wrapper already does. Raw
hardware-register access is `volatile` loads and stores to fixed addresses and
involves no calling convention at all.

**Thin units.** The Pascal `psx.pas` and Kobold `psx.kp` units are records and
`external` declarations that name the `libpsx` C entry points. They carry no
behavior. This is the standing idiom: a capability that needs no special
lowering lives in a shipped unit over `external`, not in the compiler. The
Kobold ABI already anticipates a framebuffer primitive; `kp-abi.md` uses
`procedure blit(px: buffer of integer); external` with the zero-copy
`kp_buf_map` accessor as its worked example.

The one place the units need help is an entry point that takes a pointer or
array, which Compact Pascal (no pointers) and the Kobold unit (kept
pointer-free) cannot pass. Textures solve it with an integer handle from a C
accessor, and the 3D path solves it the same way: `libpsx/gte_easy.c` wraps the
matrix- and vertex-array GTE calls in scalar-only helpers (`psx_gte_rot_xyz`
builds and loads a rotation from three angles, `psx_gte_project` projects one
vertex and `psx_gte_sx/sy/sz` read its screen position and depth back). With
those and the already-scalar ordering-table calls, a Pascal or Kobold program
projects and draws a spinning cube with no array crossing the language boundary.
`examples/psx/cube.pas` and `cube.kp` do exactly that (`test-psx-cube-lang`).

**Inline assembly.** For the rare spot that needs a hand-written instruction,
`skj-cc` and the Kobold front end both take GNU-style extended inline asm:
`asm("template" : outputs : inputs : clobbers)` with `"=r"`/`"r"`/`"+r"` operand
constraints and `%0` substitution (Kobold spells it `asm('...' : ...)` with no
`volatile` keyword, and it is an AOT feature, which is the PlayStation's path).
The Pascal front end has no inline-asm statement; there, and whenever a whole
routine is easier to write by hand, assemble a `.s` with `skj-as-mips` and link
the object (or an `skj-ar` archive) into the PS-EXE, reaching it through an
`external` declaration, exactly as `libpsx` links `gte.S` and `start_psx.S`.
`examples/psx/kasm.kp` computes a rectangle's position with extended inline asm
and draws it through the unit, so the capture verifies the result
(`test-psx-kasm`).

## Memory budget

The console has 2 MB of main RAM, 1 MB of VRAM (framebuffers and textures, not
addressable as RAM), and 512 KB of SPU RAM (sound samples). Main RAM holds the
code, the runtime, any embedded asset image, the heap and arena, the ordering
tables, and the stack. The current `start_psx.S` hardcodes a 128 KB arena in
bss, which is a bring-up default, not a budget. An early task in milestone 2 is
a documented main-RAM map: load address `0x80010000`, code and data, a sized
heap and arena, the primitive buffers, and the stack at the top of RAM less a
guard. Framebuffers live in VRAM and do not count against the 2 MB; textures
and CLUTs share that same 1 MB and are the real VRAM constraint.

## The driver pieces (`libpsx`)

Each piece is a small C module plus, where the hardware demands it, an assembly
shim. They are listed in build order.

### GPU (`gpu.c`)

The command and status ports are GP0 at `0x1F801810` and GP1 at `0x1F801814`.
The module initializes the display mode (resolution, NTSC or PAL, 15 or 24
bit), sets the drawing and display areas for double buffering, and builds
drawing primitives (flat and textured polygons, sprites, lines, tile fills) as
packets in a RAM buffer. Primitives are not written to GP0 one by one; they are
linked into an ordering table and handed to the GPU by DMA (next module). The
module owns the two-buffer swap at VSync.

### DMA (`dma.c`)

Direct memory access is how the CPU avoids touching every pixel, and it is the
single most important performance primitive on the machine. Channel 2 ships the
GPU ordering table; channel 6 clears the ordering table in reverse. The
registers are at `0x1F8010xx`. The module programs a channel's source,
block-count, and control words and waits on or interrupts off completion.

### Interrupts and VSync (`irq.c`)

A VBLANK handler paces frames and triggers the ordering-table swap. The handler
is installed either through the BIOS event system or by taking the exception
vector directly, and its body is hand-written MIPS under `.set noreorder`,
since an interrupt service routine owns its delay slots. The backend's
`SKJ_MIPS_NOREORDER` scheduler and the inline-assembly channel exist for
exactly this code (see "The noreorder scheduler" and "Inline assembly" in
`doc/tools/mips.md`).

### SPU (`spu.c`)

The sound registers begin at `0x1F801C00`. The module uploads ADPCM samples
into the 512 KB SPU RAM, keys voices on and off, and sets per-voice pitch and
volume and the main volume. `psx_spu_cd_volume` opens the CD-input mixer and the
CD-audio bit in SPUCNT so audio the CD controller produces is mixed to the main
output.

A voice can be keyed on with an ADSR envelope instead of the fixed fast-attack
default. `psx_spu_make_adsr` packs friendly 0..15 attack/decay/sustain/release
parameters into the two hardware ADSR words, and `psx_spu_play_adsr` keys a voice
on with them so its volume rises and falls over time. The SPU's reverb is driven
by a work area at the top of sound RAM and a block of coefficient registers at
`0x1F801DC0`; `psx_spu_reverb` loads a built-in preset (`PSX_REVERB_HALL` or
`PSX_REVERB_ROOM`), `psx_spu_reverb_preset` takes a caller-supplied 32-word
table, `psx_spu_voice_reverb` routes individual voices through it, and
`psx_spu_reverb_off` disables it. The envelope and reverb paths cannot be heard
by a headless run, so `test-psx-env-redux` checks the envelope bit packing by
exit code (a `make_adsr` self-check in the sample) and captures the SPU output to
confirm a reverbed voice still plays at the right pitch.

### CD audio (`cd.c`)

The CD controller can play Red Book (CD-DA) audio tracks and decode XA-ADPCM
sectors, both into the SPU CD input. `psx_cd_audio_enable` routes that input to
the main mix; `psx_cd_play_track` plays a CD-DA track; `psx_cd_play_xa` selects
an XA file/channel and streams it; `psx_cd_audio_stop` pauses. `psx_cd_track_count`
reads the table of contents. `mkcdda` builds a two-track test disc (a data track
plus a Red Book sine tone) with a CUE PCSX-Redux loads. Headless PCSX-Redux
plays CD-DA to the host sound device rather than the SPU output the capture tier
records, so the automated gate (`test-psx-cdda-redux`) checks the command path
by exit code: the TOC reports the audio track, the CD-DA track plays with an
acknowledgement, and the XA streaming commands are accepted. A host audio-monitor
capture confirms the tone by hand.

### Controllers (`pad.c`)

Controller state is read directly over SIO0, so it depends on no BIOS revision.
`psx_pad_read` returns a per-frame button mask (active-high). The read is
length-aware: the pad id's low nibble gives how many data words follow, so a
digital pad (0x41) yields the two button bytes and a DualShock in analog mode
(0x73) yields the sticks too. `psx_pad_init_analog` runs the configuration
sequence that turns analog mode on, after which `psx_pad_stick_lx` and the other
three accessors return the stick axes (0..255, 0x80 centered); a digital pad
reports all four centered.

### Memory card (`mc.c`, `mc_fs.c`)

The memory card is on SIO0 as well, selected by the 0x81 address byte. `mc.c`
reads and writes one 128-byte frame at a time over the documented card protocol
(the XOR checksum is verified on read). `mc_fs.c` builds the card's directory
format on top: frame 0 is a header and frames 1-15 describe the fifteen 8 KB
save blocks. `psx_mc_format` writes a fresh directory; `psx_mc_save` stores a
named blob across as many blocks as it needs, chaining their directory entries;
`psx_mc_load`, `psx_mc_list`, and `psx_mc_delete` round it out. The tests run
against a throwaway card image (`-memcard1`), never the user's real card, since
a format wipes it.

### Timers (`timer.c`)

The three root counters give frame timing and delays independent of the VBLANK
handler. `psx_timer_start` selects a clock source (the system clock, the system
clock divided by 8, the horizontal blank, or the GPU dot clock, depending on the
counter) and restarts the counter from zero; `psx_timer_read` returns the
16-bit count. The counters free-run with no interrupt. `timer_test` checks
counter 1 counts one NTSC frame of horizontal blanks (about 263, a
hardware-exact rate that does not wrap) and that the system clock runs far
faster; `make test-psx-timer` validates the PS-EXE and `test-psx-timer-redux`
runs the self-check on real timing hardware.

### Debug text (`debug.c`)

A text overlay for printing labels and numbers while a program runs, with no
asset to manage: an 8x8 font covering ASCII 0x20..0x7E is built into the library.
The first `psx_debug_print` (or an explicit `psx_debug_init`) rasterises the font
into a VRAM texture at (704,256), clear of the two framebuffers and of the x>=320
region `timconv` uses by default. Each character then draws as one textured
rectangle in the current frame, opaque white where the glyph is lit and
transparent elsewhere, so text sits over whatever is already on screen. The
`dbgtext` sample prints a few lines and the capture tier confirms white glyph
pixels reached the framebuffer while a corner away from the text kept the
background colour (`test-psx-dbgtext-redux`).

### Fixed-point math (`fixed.c`)

The R3051 has no FPU, and PS1 games conventionally do their math in fixed point.
This is a 16.16 fixed-point library (multiply, divide, the common trigonometric
tables). It is plain C and needs no backend work. Float code still runs through
the soft-float configuration, but fixed point is the idiom the SDK encourages
for per-frame math.

### BIOS shims (assembly)

The small set of routines that jump into the BIOS A0/B0/C0 tables, written like
`start_psx.S`'s `write`. CD-ROM and pad access that goes through the BIOS lands
here.

## Filesystem and assets

The console has no writable general-purpose filesystem and very little RAM, so
assets are read-only and either embedded or streamed. The `fs/` layer is
already a named-mount table with a backend interface (romfs, ramfs, 9pfs,
devfs), so each asset source slots in behind the same interface the C library
already calls.

**Embedded romfs, first.** `fs/romfs.c` is already freestanding-safe,
position-independent, and endian-independent, with a host-side builder
(`romfs_build_dir`). The build packs an asset directory into one blob, the blob
is linked into or loaded alongside the PS-EXE image, and the crt calls
`romfs_adopt` to mount it. The C `fs_open` and read path then works unchanged,
and the Pascal and Kobold file primitives sit on that same path. The cost is
that the whole image lives in the 2 MB main RAM beside everything else, so this
is for small or demo-sized content.

**CD-ROM isofs, second.** The memory-realistic path keeps assets on the disc
and streams 2048-byte sectors on demand into small RAM buffers. It is two
parts: a CD-ROM driver (`cd.c`, through the BIOS CD routines or the CDROM
controller at `0x1F801800`) and a read-only ISO 9660 backend
(`fs/isofs.c`) behind the same `fs/` interface romfs uses. A game mounts romfs
for boot and fallback assets and isofs for bulk content, under two names in the
one mount table. Nothing above the `fs/` interface changes.

## Verification

The existing model carries forward: `make test-mips-psexe` validates the PS-EXE
container structurally and is always on, and `make test-mips-psexe-redux` boots
a real BIOS on the PCSX-Redux emulator and self-checks by the software-exit
register. Logic that returns an exit code is tested that way.

Graphics and sound cannot be checked by an exit code. The addition is a
framebuffer-capture tier, now in place: `libpsx/tools/redux_shot.lua` drives
PCSX-Redux through its Lua API to grab the displayed framebuffer after a set
number of frames and write it to a file, and the host tool
`libpsx/tools/shotcheck.c` decodes that capture and verifies pixels. The
emulator runs with `-cli -softgpu`, which renders in software with no OpenGL
context, so no window opens and the capture runs anywhere the exit-code tier
does; `make test-psx-shot-redux` clears `DISPLAY` as a further guard.

Sound has the same kind of tier. SDL's disk audio driver writes the emulator's
output stream to a file instead of to a sound device (`SDL_AUDIO_DRIVER=disk`),
so the real SPU output is captured with no audio hardware and no window. The
host tool `libpsx/tools/wavcheck.c` measures the dominant frequency of that
capture. `make test-psx-audio-redux` plays a 440 Hz tone
(`examples/psx/audio_tone.c`) and checks the captured output is 440 Hz, so the
SPU upload, the voice key-on, and the ADPCM playback are verified, not just a
clean exit.

As with the existing Redux tier, the capture tiers are opt-in and skipped
cleanly when PCSX-Redux or a BIOS ROM is absent.

Two PCSX-Redux quirks shape the capture script. Removing a Lua event listener
from inside its own callback is a use-after-free that crashes the emulator, so
the script guards with a flag instead of removing the listener. Quitting the
emulator from Lua in `-cli` mode crashes on teardown, so the script does not
call `PCSX.quit`; it captures the frame and lets the sample reach its own clean
exit, which the frame has already been written before.

## Milestones

The numbering continues past the MIPS backend's milestone 6 (the PS-EXE target),
which this plan builds on. Milestones 1 through 4 are all C, which is where most
of the work is prototyped and proven. Milestones 5 and 6 add the other two
languages over that settled C layer.

1. **GPU, DMA, and VSync: a sprite on screen from C.** Bring up `gpu.c`,
   `dma.c`, and `irq.c` far enough to show a double-buffered sprite or flat
   quad driven from a C program, and write the main-RAM map (see "Memory
   budget"). Define the `libpsx` C API here, since it is the contract the units
   in milestone 6 bind to and the reason the C work comes first. (Done.) The
   GPU driver builds a DMA primitive linked list and ships it on channel 2;
   `irq.c`/`exc.S` install a VBLANK handler at the exception vector (the four
   COP0 instructions `skj-as-mips` does not encode are emitted as raw `.word`s)
   and `psx_vsync` sleeps on its frame counter. `examples/psx/hello_gpu.c`
   animates a flat square. Verified structurally by `make test-psx-gpu` and
   functionally by `make test-psx-gpu-redux`, which runs it on PCSX-Redux with a
   real BIOS and confirms it reaches its clean exit inside the timeout (so the
   DMA transfer and the interrupt-driven vsync both run; a hung IRQ would time
   out). `make test-psx-shot-redux` adds the golden-framebuffer check: it
   captures the displayed frame headlessly and confirms the dark-blue field and
   the yellow square `hello_gpu` draws, so the pixels are verified, not only the
   clean exit.

2. **Controllers and sound in C.** Add `pad.c` and `spu.c` so a C program reads
   the controller and plays a sample, enough for a small interactive demo.
   (Done.) `pad.c` reads a digital controller over the documented SIO0 hardware
   (no BIOS pad routines, so no BIOS-revision dependence); `spu.c` brings up the
   SPU, uploads SPU-ADPCM into sound RAM by DMA (channel 4), and keys voices on
   and off. The in-tree host converter `libpsx/tools/vagconv.c` encodes PCM into
   SPU-ADPCM and emits it as a C array. `examples/psx/input_sound.c` moves a
   d-pad-controlled square and plays a tone. Verified: the encoder by a host
   round-trip (`make test-psx-adpcm`, rms error ~19 on a sine); the PS-EXEs
   structurally (`make test-psx-m2`); and functionally on PCSX-Redux
   (`make test-psx-m2-redux`), where `pad_probe` confirms a digital pad reads
   idle (id 0x41) and `input_sound` runs the GPU/DMA/vsync/pad/SPU paths to a
   clean exit. Audio output itself is checked by the golden-audio tier (`make
   test-psx-audio-redux`, see "Testing strategy"), which captures the SPU output
   and confirms its pitch.

3. **Embedded romfs asset pipeline in C.** Pack an asset directory with
   `romfs_build_dir`, link or load the blob, `romfs_adopt` it in the crt, and
   load a sprite sheet from it through the `fs/` interface from a C program. No
   new format work; the romfs reader already exists. (Done.) The build packs a
   seed directory with `skj-mkromfs`, embeds the image with `bin2c`, and the
   sample reads it at runtime through the shared `fs/` layer. The bare-metal
   build reuses `fs/fs.c` and `fs/romfs.c` verbatim, compiled with only
   `-DFS_WITH_ROM`: the `FS_WITH_*` flags are the cut between the shared core
   (mount table, path resolution, fd space) and the host-specific backends, so
   bare metal takes the core plus romfs and none of the syscall/launcher code.
   The only platform shim is `libpsx/mem.c` (freestanding `memcpy`/`strcmp`/…,
   which the sandbox gets from its libc). `examples/psx/asset_rom.c` mounts the
   image, reads a file through `fs_open`/`fs_read`, checks its bytes, and draws
   from them. Verified structurally (`make test-psx-assets`) and functionally on
   PCSX-Redux (`make test-psx-assets-redux`): a clean exit means the romfs
   parsed, the file read back its exact bytes, and a missing path was rejected.

4. **Fixed-point math and a playable 2D sample in C.** Ship `fixed.c` and a
   small but complete 2D sample game in C (sprites, input, sound, assets from
   romfs), as the SDK's first worked example and the regression anchor. At the
   end of this milestone the whole `libpsx` API has been driven by real C and is
   ready to freeze for the units. (Done.) `fixed.c` is 16.16 fixed point:
   multiply via the runtime's 64-bit integer helpers, divide in 32-bit only
   (the PS1 runtime has no 64-bit divide helper), and sine/cosine from a
   build-time table (`libpsx/tools/mktrig.c`). `examples/psx/game.c` is a
   coin-chase game that pulls together GPU/DMA/vsync, pad, SPU, the romfs
   palette, and fixed-point motion. Verified: the math by a host self-test
   against double (`make test-psx-fixed`); the PS-EXE structurally
   (`make test-psx-game`); and functionally on PCSX-Redux
   (`make test-psx-game-redux`). The build emits no COP1 and uses no 64-bit
   divide helper. The `libpsx` C API is now exercised end to end and ready for
   the Pascal and Kobold units to bind.

5. **Front-end ports: `skj-pc-mips` and `skj-kpc-mips`.** With the C SDK proven,
   bring Pascal and Kobold to MIPS; the MIPS tier before this was C, TinC, and
   TinScheme only. Both are soft-float (the R3051 has no FPU) and ILP32, over
   the existing MIPS backend and binutils-free toolchain. (Done.) `skj-pc-mips`
   is the Pascal front end on the MIPS backend; Kobold is the two-tool split
   `skj-kpc-mips` (front end, emits a `.target mips` KPM) plus `skj-kp-mips-sf`
   (AOT codegen), which needed one line in `kpcom.pas` to stamp the mips arch
   (no seed regen). Both languages call libpsx through `external` declarations
   and share its stack calling convention, so they bind with no glue; the only
   bare-metal runtime pieces are tiny entry shims (`libpsx/pas_rt.c`,
   `libpsx/kp_rt.c`). `examples/psx/hello.pas` and `hello.kp` each draw the
   bouncing square. Verified structurally (`make test-psx-lang`) and
   functionally on PCSX-Redux (`make test-psx-lang-redux`): both run clean.
   Scope: this proves the compilers emit correct PS1 code and that programs
   using the SDK (integers plus libpsx calls, no managed runtime) run on the
   console. The full Pascal and Kobold language runtimes on bare metal (managed
   strings, dynamic arrays, maps, `write`/`readln`, the heap) are a separate
   freestanding-runtime effort, pulled in as a sample needs them; a game written
   against libpsx needs little of it.

6. **The language units: `psx.pas` and `psx.kp`, and a sample in each.** Wrap
   the now-frozen `libpsx` C API in the two thin units, binding to the C names
   through `external` with no added logic, so Pascal and Kobold lean on the C
   layer rather than reimplementing it. Port the milestone-4 sample to each
   language as proof. (Done.) `units/psx.kp` is a real Kobold `uses` unit:
   `external cdecl` declarations with thin wrappers under Kobold names, brought
   in bare with `uses psx as _`. Compact Pascal has no unit or include system,
   so `units/psx.pas` is the canonical declaration block a program copies in;
   this language difference is noted rather than papered over. A built-in
   `psx_beep` was added to libpsx so a program in any language makes a sound
   with no sample to manage. `examples/psx/game.pas` and `game.kp` are the
   coin-chase game driven entirely through the bindings (input, fixed-point
   motion, sound). Verified structurally (`make test-psx-units`) and on
   PCSX-Redux (`make test-psx-units-redux`): both run clean, the Kobold one
   through its compiled-and-linked unit module.

   The units track the C layer. As the SDK grew past this milestone, both were
   brought up to parity: they bind the semi-transparent rectangle, the
   handle-based texture draws (plain, sprite-sheet cell, and rotated), the SPU
   calls, and the CD bring-up, everything whose arguments are integers. The two
   pointer- and string-taking calls stay on the C side, since Compact Pascal has
   no pointer type and the Kobold string is not a C string on bare metal: a
   texture's TIM pointer lives in a small C accessor (`examples/psx/sprite_asset.c`)
   that returns an integer handle, and mounting a disc by path is a C call. So a
   Pascal or Kobold game draws textured, rotated, and blended sprites entirely in
   its own language through handles. `examples/psx/sprite.pas` and `sprite.kp`
   prove it: `make test-psx-sprite-lang` validates the PS-EXEs, and `make
   test-psx-sprite-lang-redux` captures each frame and checks the sprite and the
   rotated sprite render the same as the C sample.

7. **CD-ROM and isofs.** Add the `cd.c` driver and the read-only `fs/isofs.c`
   backend so assets stream from the disc instead of living in RAM. Mount romfs
   and isofs together under the one mount table. This is the memory-realistic
   asset path for a disc-sized game, and it sits behind the `fs/` interface, so
   all three languages get it without a unit change. (Done.)
   `fs/isofs.c` is a read-only ISO 9660 backend that fits the `fs_backend`
   vtable beside romfs: it parses the primary volume descriptor, walks directory
   records, and reads file extents. It reads every sector through one accessor,
   so the same parser works from a RAM image or from a sector-read callback.
   `libpsx/cd.c` drives the CDROM controller (`psx_cd_init`,
   `psx_cd_read_sector`), and `psx_cd_mount` wires that callback into isofs so a
   program reads disc files through the ordinary `fs_open` / `fs_read`.

   All three paths are verified. The host `isotest` (`make test-psx-isofs`)
   exercises the RAM mount and the streaming mount over a callback and compares a
   multi-sector file byte for byte, so the one-sector cache and reads that span
   sectors are covered. On the console, an embedded image mounts from RAM
   (`examples/psx/asset_iso.c`, `make test-psx-assets`), and `make
   test-psx-cd-redux` boots `cd_stream` with a real data disc (PCSX-Redux
   `-iso`) so the backend streams sectors through `cd.c` off the disc; it reads
   a file and the framebuffer check confirms the bytes reached the program. That
   disc boot is also the functional verification of `cd.c`, which the bare
   PS-EXE path cannot give.

8. **3D through the GTE.** The 2D primitive pipeline from milestone 1 already
   draws the screen-space quads a 3D scene needs, so 3D is mostly a vertex feed
   on top of it: the GTE (coprocessor 2) transforms and projects model vertices
   into those quads. The MIPS backend does not emit COP2 instructions, so the
   GTE is reached through a small assembly helper library (`libpsx/gte.S`), not
   new backend instruction selection. skj-as-mips grew the coprocessor-2 moves
   (`mtc2`/`mfc2`/`ctc2`/`cfc2`) and the raw `cop2` command word; its scheduler
   treats `mfc2`/`cfc2` as load-delayed like any other coprocessor read. The
   helper sets the rotation matrix (1.3.12 fixed point), the translation, and
   the projection (H, screen center), then `psx_gte_rtps` projects one vertex
   with a single RTPS. `psx_gte_rtpt` projects three vertices at once and leaves
   the GTE's screen and depth FIFOs set for `psx_gte_nclip` (the backface cross
   product) and `psx_gte_avsz3` (the ordering-table depth). `psx_gte_rotmat_x/y/z`
   build rotation matrices from the libpsx trig and `psx_gte_matmul` composes
   them. The ordering table (`ot.c`) draws a scene back to front without a sort:
   each primitive goes into the depth bucket AVSZ3 gives, and one
   `psx_display_swap` draws the frame clear and then the sorted primitives. It
   takes flat, Gouraud (per-vertex color, `psx_ot_add_gouraud3/4`), and
   texture-mapped (`psx_ot_add_tex4`) primitives, so a 3D face can be flat,
   shaded, or textured; `shade3d` verifies the Gouraud interpolation.

   The `cube` sample spins a solid cube through the whole path: each frame
   composes an X and Y rotation, and for each face projects three corners with
   RTPT, culls back faces with NCLIP, buckets the face by AVSZ3, projects the
   fourth corner, and adds the quad to the ordering table. `gte_test` and
   `gte3d` are exit-code self-checks of the projection, triangle, culling,
   depth, and matrix arithmetic, run on the GTE under PCSX-Redux the way the
   FPU/i64 self-tests check arithmetic. (Done.)

## Textures and sprites

The milestones above draw flat-colored quads. Real 2D games draw textured
sprites, so the SDK adds a texture path on top of milestone 1. (Done.)

The format is TIM, the console's own texture format: a flags word, an optional
CLUT (palette) block with a VRAM destination, then the pixel block with its own
VRAM destination. `libpsx/tools/timconv.c` converts a PPM image to a TIM in
either 8-bit paletted or 16-bit direct color. For 8-bit, an image that already
fits in the palette (256 colors by default, `--colors N` to set it) keeps every
color exactly; a richer image is reduced by a median-cut quantizer that picks a
representative palette and maps each pixel to its nearest entry. Colors become
15-bit BGR555 the way the GPU stores them, and the quantizer works on that grid
so a palette entry is always a storable color. The quantizer is checked host-side
by `make test-psx-timconv`: reconstruction error falls as the palette grows and a
low-color image stays exact.

At runtime `libpsx/tex.c` (`psx_tim_upload`) copies the CLUT and the pixels into
VRAM through the GP0 `0xA0` CPU-to-VRAM path, which is simple and runs once at
load time, and returns a `psx_texinfo` naming the texture page, palette, and
size. `psx_draw_sprite` (in `gpu.c`) selects that page in the draw-mode register
and queues a textured-rectangle primitive into the same per-frame list as the
flat primitives. VRAM is 1024x512; the two framebuffers occupy x 0..319, so
textures and palettes live at x >= 320, where `timconv` places them by default
(and `--vram`/`--clut` override for laying several out without overlap).

The whole path is checked at the pixel level. `examples/psx/tex_sprite.c`
uploads an 8-bit and a 16-bit sprite from the same four-quadrant test pattern
(red, green, blue, white) and draws them; `make test-psx-tex-redux` captures the
frame headlessly and confirms all four quadrant colors of both sprites, so the
TIM parse, the palette, the texture page, and the textured-rectangle primitive
are all verified, not just a clean exit. `make test-psx-tex` validates the
PS-EXE structurally and is always on.

Two richer draws build on that. `psx_draw_sprite_uv` draws a cell out of a
texture (the sprite-sheet case), picking a sub-rectangle by texel offset, so one
texture holds many frames. `psx_draw_rot` draws a texture as a quad with corners
rotated by an angle and scaled, the 2D transform a game uses to spin or resize a
sprite; it is a textured four-point polygon with the corners computed in
fixed-point trig (the same quad path a later 3D layer would feed). Both are
verified by `make test-psx-rot-redux` (`examples/psx/tex_rot.c`): it reads the
two sprite-sheet cells by color and checks the rotated sprite reaches a pixel
outside the reach of an axis-aligned sprite of the same size, which only a real
rotation can color. `make test-psx-rot` validates the PS-EXE structurally.

Semi-transparency is a separate flat primitive, `psx_draw_rect_alpha`: the
rectangle's color is blended with what is already in the back buffer (half and
half, additive, subtractive, or quarter), the fade, flash, and dimmed-HUD
primitive. `examples/psx/alpha.c` draws an opaque red rectangle and a half-blue
one over it, and `make test-psx-alpha-redux` checks the overlap reads as the
blend of the two while the red-only region stays opaque.

## Files (planned)

```
libpsx/gpu.c        GP0/GP1, display mode, double buffer, primitive packets
libpsx/dma.c        DMA channels 2 and 6, ordering-table transfer
libpsx/irq.c        VBLANK handler (noreorder), frame pacing, buffer swap
libpsx/spu.c        ADPCM upload, voice key-on/off, volume
libpsx/pad.c        controller read over SIO0 (digital + DualShock analog)
libpsx/mc.c         memory card frame I/O over SIO0
libpsx/mc_fs.c      memory card save-file manager (directory format)
libpsx/timer.c      root counters
libpsx/fixed.c      16.16 fixed-point math and tables
libpsx/cd.c         CD-ROM sector reads (milestone 7)
libpsx/tex.c        TIM texture upload to VRAM, sprite draw
libpsx/gte.S        GTE (COP2) helper: matrix/translation/screen, project, RTPT, NCLIP, AVSZ3 (milestone 8)
libpsx/gte_mat.c    GTE rotation-matrix builders (X/Y/Z) and 3x3 compose
libpsx/gte_easy.c   pointer-free GTE helpers for the Pascal and Kobold units
libpsx/ot.c         ordering table (depth-sorted primitive submission)
libpsx/debug.c      built-in 8x8 font, on-screen debug text overlay
libpsx/bios.S       A0/B0/C0 BIOS call shims
fs/isofs.c          read-only ISO 9660 backend (milestone 7)
libpsx/tools/redux_shot.lua  PCSX-Redux framebuffer capture (headless)
libpsx/tools/shotcheck.c     host framebuffer decoder/pixel checker
libpsx/tools/wavcheck.c      host audio-capture frequency checker
libpsx/tools/timconv.c       host PPM -> TIM texture converter
libpsx/tools/mkppm.c         host test-pattern generator
libpsx/tools/mkcdda.c        host CD-DA test-disc (CUE/BIN) generator
units/psx.pas       Compact Pascal unit over libpsx
units/psx.kp        Kobold unit over libpsx
```

The runtime stays `runtime/start_psx.S`, extended only where a game needs an
entry point it does not yet have. The backend, assembler, linker, PS-EXE
writer, and romfs are reused unchanged.
