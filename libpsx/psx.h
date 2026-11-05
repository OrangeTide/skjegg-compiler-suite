/* psx.h - libpsx, the shared PlayStation runtime SDK.
 *
 * libpsx is one C and assembly library that C, Pascal, and Kobold
 * games call to drive the original PlayStation (R3051). The full plan is in
 * doc/tools/psx-sdk.md. This header is the public C interface. Pascal and
 * Kobold reach the same entry points through thin units that bind these names.
 *
 * Milestone 1 covers the GPU. The display, DMA, interrupt, sound, controller,
 * and CD modules land in later milestones and extend this header.
 *
 * Main-RAM map (2 MB, the budget the PS-EXE loader and the crt agree on):
 *
 *     0x80010000  load base: code and data (t_addr in the PS-EXE header)
 *     ..          bss, cleared by the crt (runtime/start_psx.S)
 *     ..          the crt arenas: the bump allocator and the continuation
 *                 buffer, each declared in start_psx.S
 *     0x801ffff0  stack base, growing down (s_addr in the header), less a guard
 *
 * Framebuffers and textures live in the 1 MB of VRAM, not in this 2 MB, and
 * are addressed through the GPU rather than as memory. The SPU's 512 KB of
 * sample RAM is likewise separate. So the 2 MB holds code, data, the arenas,
 * the stack, and any embedded asset image, while the GPU and SPU RAM are the
 * real limits on how much can be on screen or playing at once.
 */

#ifndef PSX_H
#define PSX_H

#include <stdint.h>

/* The display is 320x240, 15-bit color, NTSC, double-buffered. */
#define PSX_SCREEN_W 320
#define PSX_SCREEN_H 240

/* A 16.16 fixed-point number: 16 integer bits, 16 fractional bits. The R3051
 * has no FPU, so this is the arithmetic games use for motion, scaling, and the
 * sprite rotation below. The PSX_FIX helpers and the trig are in the
 * fixed-point section further down. */
typedef int32_t fix16;

/* Bring the GPU up: reset it, set the 320x240 NTSC 15-bit mode, enable the
 * display, and install a double buffer. After this the back buffer is cleared
 * to black and ready to draw into. Call once before any other GPU call. */
void psx_gpu_init(void);

/* Set the color the back buffer is cleared to at the next psx_display_swap.
 * Components are 0..255; the GPU keeps the top 3 bits of each in 15-bit mode. */
void psx_set_bg(uint8_t r, uint8_t g, uint8_t b);

/* Draw a flat-colored rectangle into the back buffer, in screen coordinates
 * (0,0 at the top-left). Honors the drawing area, so it is clipped to the
 * screen. This is the milestone-1 sprite stand-in. */
void psx_draw_rect(int x, int y, int w, int h,
                   uint8_t r, uint8_t g, uint8_t b);

/* Semi-transparent flat rectangle: the color is blended with what is already in
 * the back buffer, for fades, flashes, and dimmed HUD panels. mode picks the
 * blend: 0 = half back + half front, 1 = back + front (additive), 2 = back -
 * front (subtractive), 3 = back + quarter front. Same coordinates and clipping
 * as psx_draw_rect. */
void psx_draw_rect_alpha(int x, int y, int w, int h,
                         uint8_t r, uint8_t g, uint8_t b, int mode);

/* Flat-shaded four-point polygon from arbitrary screen corners. Unlike
 * psx_draw_rect this is not axis-aligned, so it draws the projected faces of a
 * 3D model (the GTE path) or any quad. The corners wind as two triangles
 * (0,1,2) and (1,2,3), so pass them in that order. */
void psx_draw_poly4(int x0, int y0, int x1, int y1, int x2, int y2,
                    int x3, int y3, uint8_t r, uint8_t g, uint8_t b);

/* Show the back buffer and flip to drawing into the other one, then begin the
 * next frame's primitive list with the background clear. The DMA transfer of
 * the current frame's primitives to the GPU happens here. The usual frame is:
 * draw, then psx_vsync, then psx_display_swap. */
void psx_display_swap(void);

/* Wait for the next vertical blank. This is interrupt-driven and so requires
 * psx_irq_init to have been called; it sleeps on the VBLANK interrupt rather
 * than polling. */
void psx_vsync(void);

/* The number of vertical blanks counted since psx_irq_init, for frame timing. */
uint32_t psx_vblank_count(void);

/* --- lower-level entry points, used by the SDK and available to callers that
 * want the raw hardware. --- */

/* Enable the DMA channels libpsx uses and point the GPU at the DMA path.
 * Called by psx_gpu_init; a caller rarely needs it directly. */
void psx_dma_init(void);

/* Install the VBLANK interrupt handler, enable the VBLANK interrupt, and turn
 * on interrupts in the CPU. Call once, before psx_vsync. This takes over the
 * exception vector, so the BIOS interrupt services are unavailable afterward;
 * a later milestone that needs them (pad, CD) will cooperate with the handler
 * chain instead. */
void psx_irq_init(void);

/* DMA a GPU primitive linked list (built by the GPU module) to the GPU and
 * wait for the transfer to finish. */
void psx_dma_gpu_list(const uint32_t *head);

/* --- textures and sprites --- */

/* A texture uploaded to VRAM, as returned by psx_tim_upload. It names the GPU
 * texture page and palette the sprite draws from, plus the texel offset and
 * size of the image within that page. Pass it to psx_draw_sprite. */
typedef struct {
    uint16_t tpage;             /* GP0(E1h) texture-page attribute bits */
    uint16_t clut;              /* CLUT id (palette location); 0 for 16-bit */
    uint8_t  u, v;              /* top-left texel within the texture page */
    uint16_t w, h;             /* texture size in pixels */
} psx_texinfo;

/* Upload a TIM image (from timconv) to VRAM: the palette, if any, and the
 * pixels, each to the VRAM location baked into the TIM. On success returns 0
 * and fills *out with where the texture landed. Returns negative on a malformed
 * or unsupported TIM (only 8-bit and 16-bit images are handled). Call once per
 * texture after psx_gpu_init, before drawing; VRAM keeps the pixels, so the
 * upload is not repeated per frame. */
int psx_tim_upload(const void *tim, psx_texinfo *out);

/* Copy a rectangle of raw 16-bit pixels into VRAM at (x,y), w by h pixels, by
 * the CPU-driven GP0 path. The source is w*h halfwords in VRAM (little-endian)
 * order. For placing a texture or font bitmap that is not a TIM. */
void psx_vram_upload(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                     const void *data);

/* Draw an uploaded texture as a sprite with its top-left at screen x,y, at its
 * full size. Queued into the current frame's primitive list like psx_draw_rect,
 * so it is called each frame between psx_vsync boundaries. */
void psx_draw_sprite(const psx_texinfo *t, int x, int y);

/* Draw a w*h cell of a texture at screen x,y, the cell's top-left at texel
 * offset (u,v) within the texture. This is the sprite-sheet draw: one texture
 * holds many frames and each call picks one. (u,v) and (w,h) are in texels. */
void psx_draw_sprite_uv(const psx_texinfo *t, int x, int y,
                        int u, int v, int w, int h);

/* Draw a texture as a sprite centered at screen (cx,cy), rotated by angle (in
 * 1024 steps per full turn, as the fixed-point trig uses) and scaled by scale
 * (16.16 fixed point; PSX_FIX_ONE is 1:1). This draws a textured quad with
 * rotated corners, the 2D transform a game uses for spinning or resizing
 * sprites and the same quad path a later 3D layer feeds. */
void psx_draw_rot(const psx_texinfo *t, int cx, int cy, int angle, fix16 scale);

/* --- texture handles ---
 *
 * The functions above pass a psx_texinfo by pointer, which C uses directly. For
 * callers that cannot (Pascal has no pointers, and the Kobold unit keeps
 * the binding pointer-free), the same textures are reached by an integer handle:
 * psx_tex_upload returns one, and the handle draws take it in place of the
 * struct. The raw pointer stays on the C side, so a Pascal or Kobold game loads
 * its texture through a small C accessor that calls psx_tex_upload and hands
 * back the handle, then draws entirely in its own language. */

#define PSX_MAX_TEX 16          /* uploaded textures a program can hold at once */

/* Upload a TIM and keep it, returning a handle (0..PSX_MAX_TEX-1) for the draw
 * functions below, or a negative value if the table is full or the TIM is bad. */
int psx_tex_upload(const void *tim);

/* The info for a handle, or NULL if the handle was never returned by an upload.
 * Mainly for C code that wants the struct form after an upload. */
const psx_texinfo *psx_tex_at(int handle);

/* The handle forms of the three sprite draws, taking a handle in place of a
 * psx_texinfo pointer. A bad handle draws nothing. */
void psx_tex_draw(int handle, int x, int y);
void psx_tex_draw_uv(int handle, int x, int y, int u, int v, int w, int h);
void psx_tex_rot(int handle, int cx, int cy, int angle, fix16 scale);

/* --- debug text ---
 *
 * An on-screen text overlay with no asset to manage: the 8x8 font is built into
 * the library. The first print (or an explicit psx_debug_init) rasterises the
 * font into a VRAM texture at (704,256), clear of the framebuffers and of the
 * x>=320 region timconv uses by default; each character then draws as one
 * textured rectangle in the current frame, opaque white over whatever is there.
 * Covers ASCII 0x20..0x7E; other bytes leave a blank. */

/* Rasterise and upload the built-in font. Optional: psx_debug_print calls it on
 * first use. Call it explicitly to move the one-time upload out of the frame. */
void psx_debug_init(void);

/* Draw a NUL-terminated string with its top-left at screen x,y, 8 pixels per
 * character. Queued into the current frame like the other draws. */
void psx_debug_print(int x, int y, const char *s);

/* --- CD-ROM (milestone 7) --- */

/* Bring up the CD-ROM controller (reset, 2048-byte-sector mode). Call once
 * before reading sectors. */
void psx_cd_init(void);

/* Read one 2048-byte data sector at the given logical block address into buf.
 * Returns 0 on success or negative on error. This is the sector source the
 * ISO 9660 backend streams through on real hardware. */
int psx_cd_read_sector(uint32_t lba, void *buf);

/* Mount the disc as an ISO 9660 filesystem under the given name, read on demand
 * through the CD-ROM controller (no RAM copy of the disc). After this, a program
 * reaches disc files through the shared fs_open / fs_read, the same calls as a
 * romfs mount. Call psx_cd_init first. Returns 0 on success or negative on
 * error. */
int psx_cd_mount(const char *name);

/* CD audio. Both a CD-DA (Red Book) track and decoded XA-ADPCM feed the SPU's
 * CD input, so psx_cd_audio_enable must route that input to the main output
 * (vol is 0..0x7FFF per side) after psx_spu_init and psx_cd_init. */
void psx_cd_audio_enable(uint16_t vol);

/* Play a CD-DA audio track (1-based) to the audio output. Returns 0 on the
 * drive's acknowledgement or negative on error. */
int psx_cd_play_track(int track);

/* Stream XA-ADPCM audio starting at the given logical block address, decoding
 * only the selected XA file and channel. The controller decodes the audio into
 * the SPU as the sectors stream, so playback continues with no further calls.
 * Returns 0 on acknowledgement or negative on error. */
int psx_cd_play_xa(uint32_t lba, int file, int channel);

/* Stop CD audio playback or XA streaming and pause the drive. */
void psx_cd_audio_stop(void);

/* The number of tracks on the disc (the last track number from the TOC). A
 * data-only disc reports 1; a disc with audio tracks reports more. */
int psx_cd_track_count(void);

/* The drive's status byte (bit 1 = motor on, bit 5 = reading, bit 7 = playing
 * CD-DA). Useful for polling playback; exact bits follow the CD-ROM controller. */
int psx_cd_status(void);

/* --- controller (milestone 2) --- */

/* Digital controller buttons, as returned by psx_pad_read (1 = pressed). The
 * layout matches the controller's wire order. */
#define PSX_PAD_SELECT   0x0001u
#define PSX_PAD_L3       0x0002u
#define PSX_PAD_R3       0x0004u
#define PSX_PAD_START    0x0008u
#define PSX_PAD_UP       0x0010u
#define PSX_PAD_RIGHT    0x0020u
#define PSX_PAD_DOWN     0x0040u
#define PSX_PAD_LEFT     0x0080u
#define PSX_PAD_L2       0x0100u
#define PSX_PAD_R2       0x0200u
#define PSX_PAD_L1       0x0400u
#define PSX_PAD_R1       0x0800u
#define PSX_PAD_TRIANGLE 0x1000u
#define PSX_PAD_CIRCLE   0x2000u
#define PSX_PAD_CROSS    0x4000u
#define PSX_PAD_SQUARE   0x8000u

/* Poll controller port 1 and return the pressed-button mask (1 = pressed).
 * Zero means nothing pressed or no controller. */
uint16_t psx_pad_read(void);

/* The pad id reported by the last psx_pad_read (0x41 = digital pad, 0x73 =
 * analog pad in analog mode, 0xFF = no device). For bring-up and diagnostics. */
int psx_pad_id(void);

/* Put an analog (DualShock) controller into analog mode so psx_pad_read returns
 * its stick axes and reports id 0x73. A digital-only pad ignores it. Call once
 * after power-on, before reading sticks. */
void psx_pad_init_analog(void);

/* The analog stick axes from the last psx_pad_read, each 0..255 with 0x80 the
 * center. A digital pad (or one not in analog mode) reports all four centered. */
int psx_pad_stick_rx(void);
int psx_pad_stick_ry(void);
int psx_pad_stick_lx(void);
int psx_pad_stick_ly(void);

/* --- memory card --- */

/* The memory card holds 1024 frames of 128 bytes. These read and write one
 * frame; the save-file manager builds named saves on top of them. Both return
 * 0 on success or negative on error (no card, bad address, bad checksum). */
#define PSX_MC_FRAME_SIZE 128
#define PSX_MC_FRAMES     1024
int psx_mc_read_frame(int frame, void *buf);
int psx_mc_write_frame(int frame, const void *buf);

/* 1 if a card is present (answers a frame read), 0 otherwise. */
int psx_mc_present(void);

/* Save-file manager over the card's directory format. A save name is up to 20
 * characters. psx_mc_format writes a fresh directory. psx_mc_save stores nbytes
 * across as many 8 KB blocks as needed (overwriting any save of the same name);
 * psx_mc_load reads a named save into buf and returns the byte count (or
 * negative); psx_mc_delete frees it; psx_mc_list fills names[] (each 21 bytes)
 * and returns how many saves there are. All return negative on error. */
#define PSX_MC_NAME_MAX 20
int psx_mc_format(void);
int psx_mc_save(const char *name, const void *data, int nbytes);
int psx_mc_load(const char *name, void *buf, int bufsize);
int psx_mc_delete(const char *name);
int psx_mc_list(char names[][PSX_MC_NAME_MAX + 1], int max);

/* --- sound (milestone 2) --- */

/* A voice pitch for a sample recorded at the given rate: 0x1000 plays at
 * 44100 Hz, so a 22050 Hz sample plays at its own speed with 0x0800. */
#define PSX_SPU_PITCH_44100 0x1000u
#define PSX_SPU_PITCH_22050 0x0800u

/* Bring up the SPU: silence every voice, set the main volume, and enable
 * output. Call once before uploading or playing. */
void psx_spu_init(void);

/* Copy SPU-ADPCM sample data (from vagconv) into sound RAM at byte address
 * dest, by DMA. Sound RAM below about 0x1010 is reserved; place samples above
 * it. */
void psx_spu_upload(uint32_t dest, const void *data, uint32_t nbytes);

/* Key a voice on: play the sample at sound-RAM byte address addr, at the given
 * pitch and volume (0..0x3FFF). */
void psx_spu_play(int voice, uint32_t addr, uint16_t pitch, uint16_t vol);

/* Key a voice off. */
void psx_spu_stop(int voice);

/* Build the two ADSR envelope words from friendly 0..15 parameters: attack,
 * decay, and release are rates (0 = fast, 15 = slow); sustain is the hold
 * level (0 = quiet, 15 = full). The result packs ADSR1 in the low 16 bits and
 * ADSR2 in the high 16 bits, ready to pass to psx_spu_play_adsr. */
uint32_t psx_spu_make_adsr(int attack, int decay, int sustain, int release);

/* Key a voice on like psx_spu_play, but with an explicit ADSR envelope (from
 * psx_spu_make_adsr, or a raw value). The envelope shapes the voice's volume
 * over time instead of the fixed fast-attack default. */
void psx_spu_play_adsr(int voice, uint32_t addr, uint16_t pitch, uint16_t vol,
                       uint32_t adsr);

/* Reverb preset selector for psx_spu_reverb. */
#define PSX_REVERB_HALL 0
#define PSX_REVERB_ROOM 1

/* Turn on the SPU reverb with a built-in preset (PSX_REVERB_HALL or
 * PSX_REVERB_ROOM): place the reverb work area, load the preset coefficients,
 * open the reverb output, and set the reverb enable in SPUCNT. A voice is only
 * reverbed once psx_spu_voice_reverb enables it. */
void psx_spu_reverb(int preset);

/* Turn on reverb from a caller-supplied 32-word coefficient table, for a preset
 * other than the two built in. */
void psx_spu_reverb_preset(const uint16_t *coeffs);

/* Turn reverb off (clear the SPUCNT enable and the per-voice reverb mask). */
void psx_spu_reverb_off(void);

/* Route a voice through the reverb (on != 0) or not. */
void psx_spu_voice_reverb(int voice, int on);

/* Play a built-in tone on voice 0. The one-call way to make a sound from any
 * language, with no sample data to manage; the tone is linked into libpsx. */
void psx_beep(void);

/* Set the SPU's CD-input volume (both sides, 0..0x7FFF) and enable the CD-audio
 * path in SPUCNT, so audio the CD controller produces reaches the main output.
 * Used by psx_cd_audio_enable; call after psx_spu_init. */
void psx_spu_cd_volume(uint16_t vol);

/* --- root counters (the three hardware timers) --- */

/* Clock source for psx_timer_start, written into the counter's mode-register
 * source field.  The meaning depends on the counter: HBLANK is counter 1,
 * DOTCLK is counter 0, SYSCLK8 is counter 2, and SYSCLK is any counter. */
#define PSX_TIMER_SYSCLK  0     /* the ~33.87 MHz system clock */
#define PSX_TIMER_HBLANK  1     /* counter 1: one tick per horizontal blank */
#define PSX_TIMER_DOTCLK  1     /* counter 0: the GPU dot (pixel) clock */
#define PSX_TIMER_SYSCLK8 2     /* counter 2: the system clock divided by 8 */

/* Start counter 0, 1, or 2 free-running from the given clock source and reset
 * it to zero.  No interrupt is raised; the count wraps at 0xFFFF. */
void psx_timer_start(int counter, int source);

/* Read a counter's current 16-bit value. */
uint16_t psx_timer_read(int counter);

/* --- fixed-point math (milestone 4) --- */

/* fix16 (16.16 fixed point) is declared near the top, since the sprite rotation
 * uses it. These are the helpers over it. */
#define PSX_FIX_ONE 0x00010000
#define PSX_FIX(n)     ((fix16)((n) * PSX_FIX_ONE))   /* integer -> fix16 */
#define PSX_FIX_INT(f) ((int)((f) >> 16))             /* fix16 -> integer (floor) */

/* Fixed-point multiply and divide. Divide is 32-bit only (the PS1 runtime has
 * no 64-bit divide helper) and floors toward zero on the sign. */
fix16 psx_fmul(fix16 a, fix16 b);
fix16 psx_fdiv(fix16 a, fix16 b);

/* Sine and cosine of an angle measured in 1024 steps per full turn, returned
 * as fix16 in [-1.0, 1.0]. */
fix16 psx_fsin(int angle);
fix16 psx_fcos(int angle);

/* --- 3D through the GTE (coprocessor 2) ---
 *
 * The GTE transforms and projects 3D vertices into screen-space 2D points,
 * which then feed the same textured and flat primitives the 2D path draws. The
 * MIPS backend emits no coprocessor-2 instructions, so these are implemented in
 * assembly (libpsx/gte.S). A 3D program sets the rotation matrix, translation,
 * and projection once per object, then projects each vertex and builds quads or
 * triangles from the results. */

/* Load the 3x3 rotation matrix (row-major, nine entries in 1.3.12 fixed point,
 * so 4096 is 1.0). psx_gte_rotmat_y builds a Y-axis rotation into such an array
 * from an angle in the 1024-steps-per-turn units the fixed-point trig uses. */
void psx_gte_set_rot(const short *m);
void psx_gte_rotmat_y(short *m, int angle);

/* Set the translation added after rotation (the object's position relative to
 * the eye; z is depth, positive into the screen). */
void psx_gte_set_trans(int tx, int ty, int tz);

/* Set the projection: h is the eye-to-screen distance (larger flattens the
 * perspective), and (ofx,ofy) is the screen point the center projects to,
 * usually the screen center. */
void psx_gte_set_screen(int h, int ofx, int ofy);

/* Project one vertex through the current matrix, translation, and projection.
 * Writes out[0] = screen x, out[1] = screen y, out[2] = depth. A vertex behind
 * the eye (depth <= 0) does not project to a usable point. */
void psx_gte_rtps(int vx, int vy, int vz, int *out);

/* Project three vertices (one triangle) at once. v9 is nine shorts
 * x0,y0,z0, x1,y1,z1, x2,y2,z2; out6 receives sx0,sy0, sx1,sy1, sx2,sy2. This
 * leaves the GTE's depth and screen FIFOs set for psx_gte_nclip and
 * psx_gte_avsz3. */
void psx_gte_rtpt(const short *v9, int *out6);

/* The signed area of the three points the last psx_gte_rtpt projected. Its sign
 * is the backface test: a face wound one way is positive, the other negative,
 * so a renderer draws only the faces of the chosen sign. */
int psx_gte_nclip(void);

/* The average depth of the three vertices the last psx_gte_rtpt projected, as
 * the GTE's ordering-table Z. Use it to choose an ordering-table bucket so the
 * face is drawn in back-to-front order. */
int psx_gte_avsz3(void);

/* Rotation-matrix builders (1.3.12 fixed point) about each axis, and a 3x3
 * matrix multiply to compose rotations (out = a * b; out may alias a or b). */
void psx_gte_rotmat_x(short *m, int angle);
void psx_gte_rotmat_z(short *m, int angle);
void psx_gte_matmul(short *out, const short *a, const short *b);

/* --- ordering table (depth-sorted 3D drawing) ---
 *
 * An ordering table draws primitives back to front without a sort: clear it,
 * add each primitive at the depth bucket from psx_gte_avsz3, link it into the
 * frame, and psx_display_swap draws the clear and then the sorted primitives.
 * The 2D path (psx_draw_*) is unchanged; this is the 3D scene renderer. */

/* Start a fresh frame's table: empties every bucket and the packet pool. */
void psx_ot_clear(void);

/* Add a flat triangle or quad at depth bucket z (0 = nearest). Screen
 * coordinates come from the GTE; z is the OTZ psx_gte_avsz3 returns. */
void psx_ot_add_poly3(int x0, int y0, int x1, int y1, int x2, int y2,
                      uint8_t r, uint8_t g, uint8_t b, int z);
void psx_ot_add_poly4(int x0, int y0, int x1, int y1, int x2, int y2,
                      int x3, int y3, uint8_t r, uint8_t g, uint8_t b, int z);

/* Gouraud (per-vertex color) triangle and quad, colors packed 0xRRGGBB; the
 * GPU interpolates the colors across the face. */
void psx_ot_add_gouraud3(int x0, int y0, uint32_t c0, int x1, int y1, uint32_t c1,
                         int x2, int y2, uint32_t c2, int z);
void psx_ot_add_gouraud4(int x0, int y0, uint32_t c0, int x1, int y1, uint32_t c1,
                         int x2, int y2, uint32_t c2, int x3, int y3, uint32_t c3,
                         int z);

/* Texture-mapped quad from an uploaded texture handle, its full image mapped to
 * the four screen corners (so a 3D face can be textured). */
void psx_ot_add_tex4(int handle, int x0, int y0, int x1, int y1,
                     int x2, int y2, int x3, int y3, int z);

/* Link the table into this frame's draw list; call once before
 * psx_display_swap, after adding the primitives. */
void psx_ot_link(void);

/* Link the GPU chain's tail to another DMA list (used by the ordering table). */
void psx_gpu_chain_link(uint32_t next_addr);

/* --- pointer-free GTE helpers (for the language units) ---
 *
 * The GTE calls above pass matrices and vertices by pointer. These wrap the
 * common 3D steps so they take and return only scalars, for callers that cannot
 * pass an array (Pascal has no pointers; the Kobold unit stays
 * pointer-free). The matrix and the last projected point are kept on the C side,
 * as the texture handles keep the TIM pointer. */

/* Build a rotation from three axis angles (X, then Y, then Z, in the
 * 1024-steps-per-turn units the fixed-point trig uses), compose them, and load
 * the result as the current GTE rotation matrix. */
void psx_gte_rot_xyz(int ax, int ay, int az);

/* Project one vertex through the current matrix, translation, and projection,
 * and keep the result. Read it back with psx_gte_sx/sy/sz: screen x, screen y,
 * and depth. */
void psx_gte_project(int vx, int vy, int vz);
int psx_gte_sx(void);
int psx_gte_sy(void);
int psx_gte_sz(void);

#endif /* PSX_H */
