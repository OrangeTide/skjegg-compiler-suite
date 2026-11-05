# The PlayStation SDK

`libpsx` is a runtime SDK that lets C, Pascal, and Kobold programs drive
the original PlayStation (SCPH-100x, the R3051 at 33.8 MHz) well enough to make
games: its GPU, SPU, controllers, timers, memory card, and CD-ROM, plus 3D
through the GTE. It is one C and assembly library. Pascal and Kobold reach it
through thin units that bind the C names and add no logic of their own.

The complete function reference is in [the API reference](psx-api.md). This page
covers the architecture, the memory budget, the driver modules, the asset path,
how the SDK is tested, and what remains to build.

The SDK sits on the MIPS toolchain described in [the MIPS backend](mips.md): the
MIPS I backend, the soft-float configuration for the FPU-less R3051, the
binutils-free toolchain (`skj-as-mips`, `skj-ld-mips`, `skj-ar`), the PS-EXE
output mode (`skj-ld-mips -f ps-exe`), and the BIOS-TTY runtime
`runtime/start_psx.S`.

## Architecture

```
  C game      Pascal game         Kobold game
    │            │ uses psx          │ uses psx
    │      ┌─────┴────────┬──────────┘
    │      │  psx.pas     │  psx.kp        thin units: external
    │      │  unit        │  unit          declarations, no logic
    │      └─────┬────────┴──────────┐
    └────────────┴───────────────────┘
                 │  one C ABI (the toolkit stack convention,
                 │  which all three front ends already emit)
          ┌──────┴───────────────────────────────────────┐
          │  libpsx   (C compiled soft-float, plus asm)  │
          │  gpu  dma  irq  spu  pad  timer  cd  fixed   │
          │  gte.S  ot  tex  debug  fs glue  BIOS shims  │
          └──────┬───────────────────────────────────────┘
                 │  MMIO (volatile load/store) + BIOS calls
          start_psx.S   +   skj-ld-mips -f ps-exe
```

C includes `psx.h` and calls `libpsx` directly; Pascal and Kobold reach the same
entry points through the unit.

Two design choices keep the SDK thin.

**One calling convention.** TinC, TinScheme, the C compiler, and the Kobold AOT
path all emit the toolkit's own stack-passing convention (arguments pushed right
to left, caller pops, result in `$v0`; see "The calling convention" in
[the MIPS backend](mips.md)). So a single `libpsx.a` built with `skj-cc-mips-sf`
is callable from all three front ends with no bridging. The only code that needs
the console's o32 and BIOS register conventions is the handful of assembly shims
that jump into the BIOS, which bridge the two the way `start_psx.S`'s `write`
wrapper does. Raw hardware-register access is `volatile` loads and stores to
fixed addresses and involves no calling convention at all.

**Thin units.** `units/psx.pas` and `units/psx.kp` are `external` declarations
that name the `libpsx` C entry points and carry no behavior. This follows the
toolkit idiom: a capability that needs no special lowering lives in a shipped
unit over `external`, not in the compiler.

Pascal has no pointers and the Kobold unit stays pointer-free, so an
entry point that takes a pointer or array cannot be bound directly. Two paths
solve it. Textures use an integer handle returned by a C accessor. The 3D path
uses the scalar-only helpers in `libpsx/gte_easy.c`: `psx_gte_rot_xyz` builds
and loads a rotation from three angles, `psx_gte_project` projects one vertex,
and `psx_gte_sx`/`sy`/`sz` read its screen position and depth back. With those
and the already-scalar ordering-table calls, a Pascal or Kobold program projects
and draws a spinning cube with no array crossing the language boundary.

For a hand-written instruction, `skj-cc` and the Kobold front end both take
GNU-style extended inline asm: `asm("template" : outputs : inputs : clobbers)`
with `"=r"`/`"r"`/`"+r"` constraints and `%0` substitution. Kobold spells it
`asm('...' : ...)` with no `volatile` keyword, and it is an AOT feature, which is
the PlayStation's path. The Pascal front end has no inline-asm statement.
There, and whenever a whole routine is easier to write by hand, assemble a `.s`
with `skj-as-mips` and link the object (or an `skj-ar` archive) into the PS-EXE,
reaching it through an `external` declaration, the way `libpsx` links `gte.S` and
`start_psx.S`.

## Memory budget

The console has 2 MB of main RAM, 1 MB of VRAM (framebuffers and textures, not
addressable as RAM), and 512 KB of SPU RAM (sound samples). Main RAM holds the
code, the runtime, any embedded asset image, the heap and arena, the ordering
tables, and the stack. The main-RAM map the PS-EXE loader and the crt agree on:

```
0x80010000  load base: code and data (t_addr in the PS-EXE header)
..          bss, cleared by the crt (runtime/start_psx.S)
..          the crt arenas: the bump allocator and the continuation buffer
0x801ffff0  stack base, growing down (s_addr), less a guard
```

Framebuffers live in VRAM and do not count against the 2 MB. Textures and CLUTs
share that same 1 MB of VRAM and are the real constraint on how much can be on
screen at once; samples share the 512 KB of SPU RAM.

## The driver modules

Each module is a small C file plus, where the hardware demands it, an assembly
shim.

### GPU (`gpu.c`)

The command and status ports are GP0 at `0x1F801810` and GP1 at `0x1F801814`.
The module sets the display mode (resolution, NTSC or PAL, 15 or 24 bit), sets
the drawing and display areas for double buffering, and builds drawing
primitives (flat and textured polygons, sprites, lines, tile fills) as packets
in a RAM buffer. Primitives are linked into an ordering table and handed to the
GPU by DMA rather than written to GP0 one by one. The module owns the two-buffer
swap at VSync.

### DMA (`dma.c`)

Direct memory access is how the CPU avoids touching every pixel, and it is the
most important performance primitive on the machine. Channel 2 ships the GPU
ordering table; channel 6 clears the ordering table in reverse. The registers
are at `0x1F8010xx`. The module programs a channel's source, block-count, and
control words and waits on completion.

### Interrupts and VSync (`irq.c`)

A VBLANK handler paces frames and triggers the ordering-table swap. The handler
is installed by taking the exception vector directly, and its body is
hand-written MIPS under `.set noreorder`, since an interrupt service routine owns
its delay slots. The backend's `SKJ_MIPS_NOREORDER` scheduler and the
inline-assembly channel exist for exactly this code (see "The noreorder
scheduler" and "Inline assembly" in [the MIPS backend](mips.md)).

### SPU (`spu.c`)

The sound registers begin at `0x1F801C00`. The module uploads ADPCM samples into
the 512 KB SPU RAM, keys voices on and off, and sets per-voice pitch and volume
and the main volume. `psx_spu_cd_volume` opens the CD-input mixer and the
CD-audio bit in SPUCNT so audio the CD controller produces is mixed to the main
output.

A voice can be keyed on with an ADSR envelope instead of the fixed fast-attack
default. `psx_spu_make_adsr` packs friendly 0..15 attack, decay, sustain, and
release parameters into the two hardware ADSR words, and `psx_spu_play_adsr` keys
a voice on with them so its volume rises and falls over time. The reverb is
driven by a work area at the top of sound RAM and a block of coefficient
registers at `0x1F801DC0`. `psx_spu_reverb` loads a built-in preset
(`PSX_REVERB_HALL` or `PSX_REVERB_ROOM`), `psx_spu_reverb_preset` takes a
caller-supplied 32-word table, `psx_spu_voice_reverb` routes individual voices
through it, and `psx_spu_reverb_off` disables it.

### Controllers (`pad.c`)

Controller state is read directly over SIO0, so it depends on no BIOS revision.
`psx_pad_read` returns a per-frame button mask (active-high). The read is
length-aware: the pad id's low nibble gives how many data words follow, so a
digital pad (0x41) yields the two button bytes and a DualShock in analog mode
(0x73) yields the sticks too. `psx_pad_init_analog` runs the sequence that turns
analog mode on, after which `psx_pad_stick_lx` and the other three accessors
return the stick axes (0..255, 0x80 centered).

### Memory card (`mc.c`, `mc_fs.c`)

The memory card is on SIO0 as well, selected by the 0x81 address byte. `mc.c`
reads and writes one 128-byte frame at a time over the documented card protocol
(the XOR checksum is verified on read). `mc_fs.c` builds the card's directory
format on top: frame 0 is a header, and frames 1 to 15 describe the fifteen 8 KB
save blocks. `psx_mc_format` writes a fresh directory, `psx_mc_save` stores a
named blob across as many blocks as it needs, and `psx_mc_load`, `psx_mc_list`,
and `psx_mc_delete` round it out.

### Timers (`timer.c`)

The three root counters give frame timing and delays independent of the VBLANK
handler. `psx_timer_start` selects a clock source (the system clock, the system
clock divided by 8, the horizontal blank, or the GPU dot clock, depending on the
counter) and restarts the counter from zero; `psx_timer_read` returns the 16-bit
count. The counters free-run with no interrupt and wrap at 0xFFFF.

### Textures (`tex.c`)

The texture format is TIM, the console's own: a flags word, an optional CLUT
(palette) block with a VRAM destination, then the pixel block with its own VRAM
destination. `psx_tim_upload` copies the CLUT and the pixels into VRAM through
the GP0 `0xA0` CPU-to-VRAM path, which runs once at load time, and returns a
`psx_texinfo` naming the texture page, palette, and size. `psx_draw_sprite` (in
`gpu.c`) selects that page in the draw-mode register and queues a
textured-rectangle primitive into the same per-frame list as the flat
primitives. VRAM is 1024x512; the two framebuffers occupy x 0..319, so textures
and palettes live at x >= 320.

Two richer draws build on that. `psx_draw_sprite_uv` draws a cell out of a
texture (the sprite-sheet case), picking a sub-rectangle by texel offset, so one
texture holds many frames. `psx_draw_rot` draws a texture as a quad with corners
rotated by an angle and scaled, the 2D transform a game uses to spin or resize a
sprite; it is a textured four-point polygon with the corners computed in
fixed-point trig, the same quad path the 3D layer feeds. Semi-transparency is a
separate flat primitive, `psx_draw_rect_alpha`, which blends the rectangle's
color with the back buffer (half and half, additive, subtractive, or quarter)
for fades, flashes, and dimmed HUD panels.

For a texture reached by the integer-handle API (the form the Pascal and Kobold
units use), a program holds up to `PSX_MAX_TEX` uploaded textures and draws each
by its handle.

### Debug text (`debug.c`)

A text overlay for printing labels and numbers while a program runs, with no
asset to manage: an 8x8 font covering ASCII 0x20..0x7E is built into the library.
The first `psx_debug_print` (or an explicit `psx_debug_init`) rasterises the font
into a VRAM texture at (704,256), clear of the two framebuffers and of the
x>=320 region `timconv` uses by default. Each character then draws as one
textured rectangle in the current frame, opaque white where the glyph is lit and
transparent elsewhere, so text sits over whatever is already on screen.

### Fixed-point math (`fixed.c`)

The R3051 has no FPU, and PS1 games conventionally do their math in fixed point.
This is a 16.16 fixed-point library: multiply via the runtime's 64-bit integer
helpers, divide in 32-bit only (the PS1 runtime has no 64-bit divide helper), and
sine and cosine from a build-time table. Float code still runs through the
soft-float configuration, but fixed point is the idiom the SDK encourages for
per-frame math.

### 3D: the GTE and the ordering table (`gte.S`, `gte_mat.c`, `gte_easy.c`, `ot.c`)

The 2D primitive pipeline already draws the screen-space quads a 3D scene needs,
so 3D is mostly a vertex feed on top of it. The GTE (coprocessor 2) transforms
and projects model vertices into those quads. The MIPS backend emits no COP2
instructions, so the GTE is reached through an assembly helper
(`libpsx/gte.S`); `skj-as-mips` encodes the coprocessor-2 moves
(`mtc2`/`mfc2`/`ctc2`/`cfc2`) and the raw `cop2` command word, and its scheduler
treats `mfc2`/`cfc2` as load-delayed like any other coprocessor read.

The helper sets the rotation matrix (1.3.12 fixed point), the translation, and
the projection (eye distance, screen center), then `psx_gte_rtps` projects one
vertex with a single RTPS. `psx_gte_rtpt` projects three vertices at once and
leaves the GTE's screen and depth FIFOs set for `psx_gte_nclip` (the backface
cross product) and `psx_gte_avsz3` (the ordering-table depth).
`psx_gte_rotmat_x`/`y`/`z` build rotation matrices from the libpsx trig and
`psx_gte_matmul` composes them.

The ordering table (`ot.c`) draws a scene back to front without a sort: each
primitive goes into the depth bucket AVSZ3 gives, and one `psx_display_swap`
draws the frame clear and then the sorted primitives. It takes flat, Gouraud
(per-vertex color), and texture-mapped primitives, so a 3D face can be flat,
shaded, or textured.

### BIOS shims (assembly)

The small set of routines that jump into the BIOS A0/B0/C0 tables, written like
`start_psx.S`'s `write`. CD-ROM and pad access that goes through the BIOS lands
here.

## Filesystem and assets

The console has no writable general-purpose filesystem and little RAM, so assets
are read-only and either embedded or streamed. The `fs/` layer is a named-mount
table with a backend interface (romfs, ramfs, 9pfs, devfs, isofs), so each asset
source slots in behind the interface the C library already calls.

**Embedded romfs** is for small or demo-sized content. `fs/romfs.c` is
freestanding-safe, position-independent, and endian-independent, with a host-side
builder. The build packs an asset directory into one blob with `skj-mkromfs`, the
blob is linked into or loaded alongside the PS-EXE image, and the crt calls
`romfs_adopt` to mount it. The C `fs_open` and read path works unchanged, and the
Pascal and Kobold file primitives sit on the same path. The cost is that the
whole image lives in the 2 MB main RAM beside everything else. The bare-metal
build reuses `fs/fs.c` and `fs/romfs.c` verbatim, compiled with only
`-DFS_WITH_ROM`: the `FS_WITH_*` flags cut the shared core (mount table, path
resolution, fd space) from the host-specific backends, so bare metal takes the
core plus romfs and none of the syscall or launcher code.

**CD-ROM isofs** is the memory-realistic path: it keeps assets on the disc and
streams 2048-byte sectors on demand into small RAM buffers. `fs/isofs.c` is a
read-only ISO 9660 backend that fits the `fs_backend` vtable beside romfs. It
parses the primary volume descriptor, walks directory records, and reads file
extents through one sector accessor, so the same parser works from a RAM image or
from a sector-read callback. `libpsx/cd.c` drives the CDROM controller
(`psx_cd_read_sector`), and `psx_cd_mount` wires that callback into isofs so a
program reads disc files through the ordinary `fs_open` and `fs_read`. A game can
mount romfs for boot and fallback assets and isofs for bulk content under two
names in the one mount table, and nothing above the `fs/` interface changes.

## Testing

Logic that returns an exit code is tested by the PS-EXE tiers.
`make test-mips-psexe` validates the PS-EXE container structurally and is always
on; `make test-mips-psexe-redux` boots a real BIOS on the PCSX-Redux emulator and
self-checks by the software-exit register. The GTE arithmetic, the fixed-point
math, the TIM quantizer, the ADPCM encoder, and the isofs reader each have a
host or exit-code self-check.

Graphics and sound cannot be checked by an exit code, so there are capture tiers.
`libpsx/tools/redux_shot.lua` drives PCSX-Redux through its Lua API to grab the
displayed framebuffer after a set number of frames, and the host tool
`libpsx/tools/shotcheck.c` decodes that capture and verifies pixels. The emulator
runs with `-cli -softgpu`, which renders in software with no OpenGL context, so
no window opens and the capture runs anywhere the exit-code tier does. Sound has
the same kind of tier: SDL's disk audio driver writes the emulator's output
stream to a file (`SDL_AUDIO_DRIVER=disk`), and `libpsx/tools/wavcheck.c` measures
the dominant frequency, so an SPU upload, voice key-on, and ADPCM playback are
verified by pitch rather than by a clean exit. The capture tiers are opt-in and
skipped cleanly when PCSX-Redux or a BIOS ROM is absent.

Two PCSX-Redux quirks shape the capture script. Removing a Lua event listener
from inside its own callback is a use-after-free that crashes the emulator, so
the script guards with a flag instead of removing the listener. Quitting the
emulator from Lua in `-cli` mode crashes on teardown, so the script does not call
`PCSX.quit`; it captures the frame and lets the sample reach its own clean exit,
by which point the frame is already written.

## Status and roadmap

The SDK is feature-complete for 2D games and covers basic 3D. Working today, in
all three languages: the GPU and double-buffered display, DMA, VSync, flat and
textured sprites (plain, sprite-sheet cell, and rotated), semi-transparency, the
SPU with ADSR and reverb, CD-DA and XA audio, the controller (digital and analog)
and memory card, the three timers, 16.16 fixed-point math, debug text, and 3D
through the GTE and the ordering table. The Pascal and Kobold `psx` units are at
2D and 3D parity for every integer-argument entry point; the pointer- and
string-taking calls stay on the C side.

What remains to build:

- **Full managed language runtimes on bare metal.** A game written against
  `libpsx` uses integers plus library calls and needs little runtime, which is
  what the samples do today. The complete Pascal and Kobold runtimes (managed
  strings, dynamic arrays, maps, `write`/`readln`, the heap) are a separate
  freestanding-runtime effort, pulled in as a sample needs them rather than
  shipped whole.

The backend, assembler, linker, PS-EXE writer, and romfs are reused unchanged;
`runtime/start_psx.S` is extended only where a game needs an entry point it does
not yet have.

## Files

```
libpsx/gpu.c        GP0/GP1, display mode, double buffer, primitive packets
libpsx/dma.c        DMA channels 2 and 6, ordering-table transfer
libpsx/irq.c        VBLANK handler (noreorder), frame pacing, buffer swap
libpsx/spu.c        ADPCM upload, voice key-on/off, volume, ADSR, reverb
libpsx/pad.c        controller read over SIO0 (digital + DualShock analog)
libpsx/mc.c         memory card frame I/O over SIO0
libpsx/mc_fs.c      memory card save-file manager (directory format)
libpsx/timer.c      root counters
libpsx/fixed.c      16.16 fixed-point math and tables
libpsx/cd.c         CD-ROM sector reads and CD/XA audio
libpsx/tex.c        TIM texture upload to VRAM, sprite draw
libpsx/debug.c      built-in 8x8 font, on-screen debug text overlay
libpsx/gte.S        GTE (COP2) helper: matrix/translation/screen, project, RTPT, NCLIP, AVSZ3
libpsx/gte_mat.c    GTE rotation-matrix builders (X/Y/Z) and 3x3 compose
libpsx/gte_easy.c   pointer-free GTE helpers for the Pascal and Kobold units
libpsx/ot.c         ordering table (depth-sorted primitive submission)
libpsx/bios.S       A0/B0/C0 BIOS call shims
fs/isofs.c          read-only ISO 9660 backend
libpsx/tools/redux_shot.lua  PCSX-Redux framebuffer capture (headless)
libpsx/tools/shotcheck.c     host framebuffer decoder/pixel checker
libpsx/tools/wavcheck.c      host audio-capture frequency checker
libpsx/tools/timconv.c       host PPM -> TIM texture converter
libpsx/tools/vagconv.c       host PCM -> SPU-ADPCM converter
libpsx/tools/mkcdda.c        host CD-DA test-disc (CUE/BIN) generator
units/psx.pas       Pascal unit over libpsx
units/psx.kp        Kobold unit over libpsx
```
