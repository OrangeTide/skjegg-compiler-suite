# The PlayStation SDK API reference

This is the complete function reference for `libpsx`, the shared PlayStation
runtime SDK. For the design, the memory budget, the driver pieces, and the
build and verification flow, see [the PlayStation SDK overview](psx-sdk.md).
This page documents every public entry point.

The authoritative declaration is `libpsx/psx.h`. The signatures below match it;
the prose is the contract. A function with no documented return value returns
nothing.

## Calling from C, Pascal, and Kobold

One library backs all three languages. The entry points are plain C with the
toolkit stack calling convention, and the Pascal and Kobold bindings are thin
units that re-export the same code, adding no logic.

- **C** includes `libpsx/psx.h` and calls the `psx_*` names directly. This is
  the full surface; everything on this page is reachable from C.
- **Pascal** does `uses psx` (`units/psx.pas`) and calls the `psx_*` names
  verbatim, declared as externals. The unit exposes the pointer-free subset (no
  function that takes a C pointer or struct).
- **Kobold** does `uses psx as _` (`units/psx.kp`) and calls PascalCase names:
  `psx_gpu_init` becomes `GpuInit`, `psx_draw_rect` becomes `DrawRect`, and so
  on. The unit exposes the same pointer-free subset.

Pascal has no pointers and the Kobold unit stays pointer-free, so the handful of
entry points that take a C pointer (a `psx_texinfo`, a matrix, a raw buffer) are
C-only. Textures reach the other two languages through the integer-handle API
(`psx_tex_upload` and the `psx_tex_*` draws), and 3D reaches them through the
scalar-only GTE helpers (`psx_gte_rot_xyz`, `psx_gte_project`, and the readback
functions). A Pascal or Kobold game that needs a pointer-taking call wraps it in
a small C accessor and links that in.

The Kobold-name column below gives the name the Kobold unit exports, or "C only"
when the entry point is not in the unit.

## Types and constants

```c
typedef int32_t fix16;   /* 16.16 fixed point: 16 integer bits, 16 fractional */
```

| Constant | Value | Meaning |
|----------|-------|---------|
| `PSX_SCREEN_W` | 320 | display width in pixels |
| `PSX_SCREEN_H` | 240 | display height in pixels |
| `PSX_FIX_ONE` | 0x00010000 | 1.0 in `fix16` (Kobold: `FixOne`) |
| `PSX_MAX_TEX` | 16 | textures a program can hold uploaded at once |

```c
#define PSX_FIX(n)     ((fix16)((n) * PSX_FIX_ONE))  /* integer -> fix16 */
#define PSX_FIX_INT(f) ((int)((f) >> 16))            /* fix16 -> integer, floor */
```

Angles throughout the SDK (sprite rotation, the trig, the GTE helpers) are
measured in **1024 steps per full turn**, not radians or degrees.

```c
typedef struct {
    uint16_t tpage;    /* GP0(E1h) texture-page attribute bits */
    uint16_t clut;     /* CLUT id (palette location); 0 for 16-bit */
    uint8_t  u, v;     /* top-left texel within the texture page */
    uint16_t w, h;     /* texture size in pixels */
} psx_texinfo;
```

## GPU and display

| C | Kobold | Summary |
|---|--------|---------|
| `void psx_gpu_init(void)` | `GpuInit` | Bring the GPU up and install the double buffer. |
| `void psx_set_bg(uint8_t r, uint8_t g, uint8_t b)` | `SetBg` | Set the clear color for the next swap. |
| `void psx_draw_rect(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b)` | `DrawRect` | Flat-colored rectangle into the back buffer. |
| `void psx_draw_rect_alpha(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b, int mode)` | `DrawRectAlpha` | Semi-transparent rectangle, blended. |
| `void psx_draw_poly4(int x0, int y0, int x1, int y1, int x2, int y2, int x3, int y3, uint8_t r, uint8_t g, uint8_t b)` | C only | Flat four-point polygon from arbitrary corners. |
| `void psx_display_swap(void)` | `DisplaySwap` | Show the back buffer, flip, and start the next frame. |
| `void psx_vsync(void)` | `Vsync` | Sleep until the next vertical blank. |
| `uint32_t psx_vblank_count(void)` | `VblankCount` | Vertical blanks counted since `psx_irq_init`. |

`psx_gpu_init` resets the GPU, selects 320x240 NTSC 15-bit mode, enables the
display, installs a double buffer, and clears the back buffer to black. Call it
once before any other GPU call.

`psx_set_bg` components are 0..255; the GPU keeps the top 3 bits of each in
15-bit mode. The color takes effect at the next `psx_display_swap`.

`psx_draw_rect` uses screen coordinates with (0,0) at the top-left, and is
clipped to the drawing area.

`psx_draw_rect_alpha` `mode` picks the blend: 0 = half back + half front, 1 =
back + front (additive), 2 = back - front (subtractive), 3 = back + quarter
front. Coordinates and clipping match `psx_draw_rect`. The Kobold unit names the
modes `BlendHalf`, `BlendAdd`, `BlendSub`, `BlendQuarter`.

`psx_draw_poly4` is not axis-aligned, so it draws projected 3D faces or any
quad. The corners wind as two triangles (0,1,2) and (1,2,3); pass them in that
order.

`psx_vsync` is interrupt-driven and requires `psx_irq_init` first; it sleeps on
the VBLANK interrupt rather than polling. The usual frame is: draw, then
`psx_vsync`, then `psx_display_swap`.

### Low-level GPU, DMA, and interrupts

These back the calls above and are available to a program that wants the raw
hardware. All are C-only.

| C | Summary |
|---|---------|
| `void psx_dma_init(void)` | Enable the DMA channels and point the GPU at the DMA path. Called by `psx_gpu_init`. |
| `void psx_irq_init(void)` | Install the VBLANK handler, enable the interrupt, and turn on CPU interrupts. Call once before `psx_vsync`. |
| `void psx_dma_gpu_list(const uint32_t *head)` | DMA a GPU primitive linked list to the GPU and wait for it to finish. |
| `void psx_gpu_chain_link(uint32_t next_addr)` | Link the GPU chain's tail to another DMA list (used by the ordering table). |

`psx_irq_init` takes over the exception vector, so the BIOS interrupt services
are unavailable afterward; the pad and CD modules cooperate with the handler
chain instead.

## Textures and sprites

The struct-based calls are C-only; the handle-based calls (`psx_tex_*`) are the
form Pascal and Kobold use.

| C | Kobold | Summary |
|---|--------|---------|
| `int psx_tim_upload(const void *tim, psx_texinfo *out)` | C only | Upload a TIM image to VRAM; fill `*out`. |
| `void psx_vram_upload(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const void *data)` | C only | Copy raw 16-bit pixels into VRAM. |
| `void psx_draw_sprite(const psx_texinfo *t, int x, int y)` | C only | Draw a texture at full size, top-left at (x,y). |
| `void psx_draw_sprite_uv(const psx_texinfo *t, int x, int y, int u, int v, int w, int h)` | C only | Draw a w*h cell at texel (u,v): the sprite-sheet draw. |
| `void psx_draw_rot(const psx_texinfo *t, int cx, int cy, int angle, fix16 scale)` | C only | Draw a texture centered at (cx,cy), rotated and scaled. |
| `int psx_tex_upload(const void *tim)` | C only | Upload a TIM and keep it; return a handle (0..`PSX_MAX_TEX`-1). |
| `const psx_texinfo *psx_tex_at(int handle)` | C only | The info for a handle, or NULL if the handle is unused. |
| `void psx_tex_draw(int handle, int x, int y)` | `TexDraw` | Handle form of `psx_draw_sprite`. |
| `void psx_tex_draw_uv(int handle, int x, int y, int u, int v, int w, int h)` | `TexDrawUv` | Handle form of `psx_draw_sprite_uv`. |
| `void psx_tex_rot(int handle, int cx, int cy, int angle, fix16 scale)` | `TexRot` | Handle form of `psx_draw_rot`. |

`psx_tim_upload` returns 0 and fills `*out` on success, or a negative value on a
malformed or unsupported TIM (only 8-bit and 16-bit images are handled). Call it
once per texture after `psx_gpu_init` and before drawing; VRAM keeps the pixels,
so the upload is not repeated per frame.

`psx_vram_upload` takes `w*h` halfwords in VRAM (little-endian) order, for a
texture or font bitmap that is not a TIM.

`psx_draw_rot` `angle` is in 1024 steps per turn; `scale` is `fix16` with
`PSX_FIX_ONE` for 1:1. It draws a textured quad with rotated corners.

`psx_tex_upload` returns a negative value if the table is full or the TIM is
bad. A bad handle in any `psx_tex_*` draw draws nothing. This is the path a
Pascal or Kobold game uses: a small C accessor calls `psx_tex_upload`, hands
back the integer handle, and the game then draws entirely in its own language.

## Debug text

A built-in 8x8 font overlay, with no asset to manage. The font rasterises into a
VRAM texture at (704,256), clear of the framebuffers; each character draws as
one textured rectangle, opaque white. Covers ASCII 0x20..0x7E; other bytes leave
a blank. Both calls are C-only.

| C | Summary |
|---|---------|
| `void psx_debug_init(void)` | Rasterise and upload the font. Optional; `psx_debug_print` calls it on first use. |
| `void psx_debug_print(int x, int y, const char *s)` | Draw a NUL-terminated string at (x,y), 8 pixels per character. |

Call `psx_debug_init` explicitly to move the one-time upload out of the frame
loop.

## Controllers

| C | Kobold | Summary |
|---|--------|---------|
| `uint16_t psx_pad_read(void)` | `PadRead` | Poll port 1; return the pressed-button mask (1 = pressed). |
| `int psx_pad_id(void)` | `PadId` | The pad id from the last read (0x41 digital, 0x73 analog, 0xFF none). |
| `void psx_pad_init_analog(void)` | C only | Put a DualShock into analog mode. Call once after power-on. |
| `int psx_pad_stick_rx(void)` | C only | Right stick X from the last read (0..255, 0x80 center). |
| `int psx_pad_stick_ry(void)` | C only | Right stick Y. |
| `int psx_pad_stick_lx(void)` | C only | Left stick X. |
| `int psx_pad_stick_ly(void)` | C only | Left stick Y. |

`psx_pad_read` returns 0 when nothing is pressed or no controller is attached.
The button mask bits are:

| Bit | C macro | Kobold |
|-----|---------|--------|
| 0x0001 | `PSX_PAD_SELECT` | `PadSelect` |
| 0x0002 | `PSX_PAD_L3` | C only |
| 0x0004 | `PSX_PAD_R3` | C only |
| 0x0008 | `PSX_PAD_START` | `PadStart` |
| 0x0010 | `PSX_PAD_UP` | `PadUp` |
| 0x0020 | `PSX_PAD_RIGHT` | `PadRight` |
| 0x0040 | `PSX_PAD_DOWN` | `PadDown` |
| 0x0080 | `PSX_PAD_LEFT` | `PadLeft` |
| 0x0100 | `PSX_PAD_L2` | C only |
| 0x0200 | `PSX_PAD_R2` | C only |
| 0x0400 | `PSX_PAD_L1` | `PadL1` |
| 0x0800 | `PSX_PAD_R1` | `PadR1` |
| 0x1000 | `PSX_PAD_TRIANGLE` | `PadTriangle` |
| 0x2000 | `PSX_PAD_CIRCLE` | `PadCircle` |
| 0x4000 | `PSX_PAD_CROSS` | `PadCross` |
| 0x8000 | `PSX_PAD_SQUARE` | `PadSquare` |

A digital pad, or one not in analog mode, reports all four stick axes centered
at 0x80.

## Memory card

The card holds 1024 frames of 128 bytes. The frame calls are the raw layer; the
save-file manager builds named saves on top. All are C-only (the named-save
calls take strings and buffers).

| C | Summary |
|---|---------|
| `int psx_mc_read_frame(int frame, void *buf)` | Read one 128-byte frame. |
| `int psx_mc_write_frame(int frame, const void *buf)` | Write one 128-byte frame. |
| `int psx_mc_present(void)` | 1 if a card is present, 0 otherwise. |
| `int psx_mc_format(void)` | Write a fresh directory. |
| `int psx_mc_save(const char *name, const void *data, int nbytes)` | Store a named save across as many 8 KB blocks as needed. |
| `int psx_mc_load(const char *name, void *buf, int bufsize)` | Read a named save into `buf`; return the byte count. |
| `int psx_mc_delete(const char *name)` | Delete a named save. |
| `int psx_mc_list(char names[][PSX_MC_NAME_MAX + 1], int max)` | Fill `names[]` with the saves; return how many. |

Relevant sizes: `PSX_MC_FRAME_SIZE` 128, `PSX_MC_FRAMES` 1024,
`PSX_MC_NAME_MAX` 20 (so each name buffer is 21 bytes). The frame calls return 0
on success or negative on error (no card, bad address, bad checksum). The
save-file calls return negative on error; `psx_mc_save` overwrites any save of
the same name.

## Sound (SPU)

| C | Kobold | Summary |
|---|--------|---------|
| `void psx_spu_init(void)` | `SpuInit` | Silence every voice, set the main volume, enable output. |
| `void psx_spu_upload(uint32_t dest, const void *data, uint32_t nbytes)` | C only | DMA SPU-ADPCM sample data into sound RAM. |
| `void psx_spu_play(int voice, uint32_t addr, uint16_t pitch, uint16_t vol)` | `SpuPlay` | Key a voice on at the given pitch and volume. |
| `void psx_spu_stop(int voice)` | `SpuStop` | Key a voice off. |
| `uint32_t psx_spu_make_adsr(int attack, int decay, int sustain, int release)` | C only | Build the two ADSR envelope words from 0..15 parameters. |
| `void psx_spu_play_adsr(int voice, uint32_t addr, uint16_t pitch, uint16_t vol, uint32_t adsr)` | C only | Key a voice on with an explicit ADSR envelope. |
| `void psx_spu_reverb(int preset)` | C only | Turn on reverb with a built-in preset. |
| `void psx_spu_reverb_preset(const uint16_t *coeffs)` | C only | Turn on reverb from a 32-word coefficient table. |
| `void psx_spu_reverb_off(void)` | C only | Turn reverb off. |
| `void psx_spu_voice_reverb(int voice, int on)` | C only | Route a voice through the reverb, or not. |
| `void psx_beep(void)` | `Beep` | Play a built-in tone on voice 0. |
| `void psx_spu_cd_volume(uint16_t vol)` | C only | Set the SPU's CD-input volume and enable the CD-audio path. |

Pitch is a voice's playback rate: `PSX_SPU_PITCH_44100` (0x1000) plays a sample
at 44100 Hz; `PSX_SPU_PITCH_22050` (0x0800) plays a 22050 Hz sample at its own
speed (Kobold: `SpuPitch44100`, `SpuPitch22050`). Volume in `psx_spu_play` is
0..0x3FFF. Sound RAM below about 0x1010 is reserved; place samples above it.

`psx_spu_make_adsr` takes friendly 0..15 parameters: attack, decay, and release
are rates (0 = fast, 15 = slow); sustain is the hold level (0 = quiet, 15 =
full). The result packs ADSR1 in the low 16 bits and ADSR2 in the high 16 bits,
ready for `psx_spu_play_adsr`, which shapes the voice's volume over time instead
of the fixed fast-attack default.

The reverb preset selectors are `PSX_REVERB_HALL` (0) and `PSX_REVERB_ROOM` (1).
`psx_spu_reverb` places the work area, loads the coefficients, opens the reverb
output, and sets the enable in SPUCNT; a voice is only reverbed once
`psx_spu_voice_reverb` enables it.

`psx_beep` is the one-call way to make a sound from any language, with no sample
data to manage.

## CD-ROM

The disc calls are C-only. `psx_cd_mount` takes a path string and integrates
with the shared `fs_open`/`fs_read`, the same calls as a romfs mount.

| C | Kobold | Summary |
|---|--------|---------|
| `void psx_cd_init(void)` | `CdInit` | Bring up the drive (reset, 2048-byte-sector mode). |
| `int psx_cd_read_sector(uint32_t lba, void *buf)` | C only | Read one 2048-byte data sector. |
| `int psx_cd_mount(const char *name)` | C only | Mount the disc as an ISO 9660 filesystem under `name`. |
| `void psx_cd_audio_enable(uint16_t vol)` | C only | Route the SPU CD input to the main output (vol 0..0x7FFF per side). |
| `int psx_cd_play_track(int track)` | C only | Play a CD-DA audio track (1-based). |
| `int psx_cd_play_xa(uint32_t lba, int file, int channel)` | C only | Stream XA-ADPCM audio from `lba` for one file and channel. |
| `void psx_cd_audio_stop(void)` | C only | Stop playback or XA streaming and pause the drive. |
| `int psx_cd_track_count(void)` | C only | Number of tracks on the disc (1 for a data-only disc). |
| `int psx_cd_status(void)` | C only | The drive status byte (bit 1 motor on, bit 5 reading, bit 7 playing CD-DA). |

The sector and track calls return 0 on success (or the drive's acknowledgement)
or negative on error. `psx_cd_mount` reads on demand through the controller, with
no RAM copy of the disc; call `psx_cd_init` first. `psx_cd_audio_enable` needs
`psx_spu_init` and `psx_cd_init` done first, since it routes the SPU CD input.
XA playback continues with no further calls once started.

## Timers (root counters)

The three hardware counters, C-only.

| C | Summary |
|---|---------|
| `void psx_timer_start(int counter, int source)` | Start counter 0/1/2 free-running from `source`, reset to zero. |
| `uint16_t psx_timer_read(int counter)` | Read a counter's current 16-bit value. |

No interrupt is raised; the count wraps at 0xFFFF. The clock sources, written
into the counter's mode-register source field, depend on the counter:

| Source | Value | Meaning |
|--------|-------|---------|
| `PSX_TIMER_SYSCLK` | 0 | the ~33.87 MHz system clock (any counter) |
| `PSX_TIMER_HBLANK` | 1 | one tick per horizontal blank (counter 1) |
| `PSX_TIMER_DOTCLK` | 1 | the GPU dot (pixel) clock (counter 0) |
| `PSX_TIMER_SYSCLK8` | 2 | the system clock divided by 8 (counter 2) |

## Fixed-point math

| C | Kobold | Summary |
|---|--------|---------|
| `fix16 psx_fmul(fix16 a, fix16 b)` | `Fmul` | Fixed-point multiply. |
| `fix16 psx_fdiv(fix16 a, fix16 b)` | C only | Fixed-point divide (32-bit, floors toward zero on sign). |
| `fix16 psx_fsin(int angle)` | `Fsin` | Sine of an angle (1024 steps per turn), `fix16` in [-1.0, 1.0]. |
| `fix16 psx_fcos(int angle)` | `Fcos` | Cosine, same units and range. |

The R3051 has no FPU, so `fix16` (16.16) is the arithmetic games use for motion,
scaling, and rotation. `psx_fdiv` is 32-bit only, as the PS1 runtime has no
64-bit divide helper.

## 3D through the GTE

The GTE (coprocessor 2) transforms and projects 3D vertices into screen-space 2D
points, which feed the same primitives the 2D path draws. The MIPS backend emits
no coprocessor-2 instructions, so these are implemented in assembly
(`libpsx/gte.S`). The matrix- and vertex-array calls are C-only; the
scalar-only helpers below them are the form Pascal and Kobold use.

| C | Kobold | Summary |
|---|--------|---------|
| `void psx_gte_set_rot(const short *m)` | C only | Load the 3x3 rotation matrix (row-major, 1.3.12 fixed point, 4096 = 1.0). |
| `void psx_gte_set_trans(int tx, int ty, int tz)` | `GteSetTrans` | Set the translation added after rotation (z into the screen). |
| `void psx_gte_set_screen(int h, int ofx, int ofy)` | `GteSetScreen` | Set projection: eye-to-screen distance `h`, projection center (ofx,ofy). |
| `void psx_gte_rtps(int vx, int vy, int vz, int *out)` | C only | Project one vertex; `out` gets screen x, screen y, depth. |
| `void psx_gte_rtpt(const short *v9, int *out6)` | C only | Project three vertices at once (sets up `nclip`/`avsz3`). |
| `int psx_gte_nclip(void)` | C only | Signed area of the last `rtpt` triangle (the backface test). |
| `int psx_gte_avsz3(void)` | C only | Average depth of the last `rtpt` triangle, as the ordering-table Z. |
| `void psx_gte_rotmat_x(short *m, int angle)` | C only | Build an X-axis rotation matrix. |
| `void psx_gte_rotmat_y(short *m, int angle)` | C only | Build a Y-axis rotation matrix. |
| `void psx_gte_rotmat_z(short *m, int angle)` | C only | Build a Z-axis rotation matrix. |
| `void psx_gte_matmul(short *out, const short *a, const short *b)` | C only | 3x3 matrix multiply (`out = a*b`; `out` may alias). |
| `void psx_gte_rot_xyz(int ax, int ay, int az)` | `GteRotXyz` | Build and load a rotation from three axis angles (X, then Y, then Z). |
| `void psx_gte_project(int vx, int vy, int vz)` | `GteProject` | Project one vertex and keep the result. |
| `int psx_gte_sx(void)` | `GteSx` | Screen x of the last `psx_gte_project`. |
| `int psx_gte_sy(void)` | `GteSy` | Screen y. |
| `int psx_gte_sz(void)` | `GteSz` | Depth. |

Matrix entries are 1.3.12 fixed point (4096 = 1.0). Angles are in 1024 steps per
turn. A vertex behind the eye (depth <= 0) does not project to a usable point.
The `nclip` sign is the backface test: a face wound one way is positive, the
other negative, so a renderer draws only faces of the chosen sign. `avsz3`
chooses an ordering-table bucket for back-to-front drawing.

The scalar helpers keep the matrix and the last projected point on the C side,
the way the texture handles keep the TIM pointer. A 3D program sets the view
once (`psx_gte_set_screen`, `psx_gte_set_trans`), builds a rotation
(`psx_gte_rot_xyz`), projects each vertex (`psx_gte_project`), and reads the
screen position and depth back (`psx_gte_sx`/`sy`/`sz`). See
`examples/psx/cube.pas` and `examples/psx/cube.kp`.

## Ordering table

An ordering table draws primitives back to front without a sort: clear it, add
each primitive at the depth bucket from `psx_gte_avsz3`, link it into the frame,
and `psx_display_swap` draws the clear and then the sorted primitives. The 2D
path is unchanged; this is the 3D scene renderer.

| C | Kobold | Summary |
|---|--------|---------|
| `void psx_ot_clear(void)` | `OtClear` | Start a fresh frame's table: empty every bucket and the packet pool. |
| `void psx_ot_add_poly3(int x0, int y0, int x1, int y1, int x2, int y2, uint8_t r, uint8_t g, uint8_t b, int z)` | C only | Add a flat triangle at depth bucket `z`. |
| `void psx_ot_add_poly4(int x0, int y0, int x1, int y1, int x2, int y2, int x3, int y3, uint8_t r, uint8_t g, uint8_t b, int z)` | `OtAddPoly4` | Add a flat quad. |
| `void psx_ot_add_gouraud3(int x0, int y0, uint32_t c0, int x1, int y1, uint32_t c1, int x2, int y2, uint32_t c2, int z)` | C only | Add a Gouraud triangle (colors 0xRRGGBB). |
| `void psx_ot_add_gouraud4(int x0, int y0, uint32_t c0, int x1, int y1, uint32_t c1, int x2, int y2, uint32_t c2, int x3, int y3, uint32_t c3, int z)` | `OtAddGouraud4` | Add a Gouraud quad. |
| `void psx_ot_add_tex4(int handle, int x0, int y0, int x1, int y1, int x2, int y2, int x3, int y3, int z)` | C only | Add a textured quad from an uploaded handle. |
| `void psx_ot_link(void)` | `OtLink` | Link the table into this frame's draw list. |

`z` is 0 for nearest; it is the OTZ `psx_gte_avsz3` returns. Gouraud colors are
packed 0xRRGGBB and the GPU interpolates them across the face. `psx_ot_add_tex4`
maps the full image of a handle's texture to the four screen corners. Call
`psx_ot_link` once, after adding the primitives and before `psx_display_swap`.

## See also

- [The PlayStation SDK overview](psx-sdk.md): design, memory budget, driver
  pieces, asset tools, verification, and milestones.
- [The MIPS backend](mips-backend.md): the R3000/R3051 code generator and the
  PS-EXE output path.
- `examples/psx/`: worked examples in C, Pascal, and Kobold.
