{ psx.pas - the Compact Pascal binding to the libpsx PlayStation SDK
  (milestone 6).

  Compact Pascal has no unit or include system, so this is not a compilable
  unit: it is the canonical declaration block for the libpsx entry points, to
  be copied into a Pascal program's declaration section. (The Kobold binding,
  units/psx.kp, is a real `uses` unit; Compact Pascal's lack of modules is the
  one place the two languages differ here.) Pascal and libpsx share the toolkit
  stack calling convention, so these bind with no glue.

  Button bits for psx_pad_read (1 = pressed):
    Select=1 Start=8 Up=16 Right=32 Down=64 Left=128
    L1=1024 R1=2048 Triangle=4096 Circle=8192 Cross=16384 Square=-32768 }

procedure psx_gpu_init; external;
procedure psx_irq_init; external;
procedure psx_set_bg(r, g, b: integer); external;
procedure psx_draw_rect(x, y, w, h, r, g, b: integer); external;
procedure psx_draw_rect_alpha(x, y, w, h, r, g, b, mode: integer); external;
procedure psx_vsync; external;
procedure psx_display_swap; external;
function  psx_pad_read: integer; external;
function  psx_pad_id: integer; external;
function  psx_vblank_count: integer; external;
procedure psx_beep; external;
function  psx_fmul(a, b: integer): integer; external;
function  psx_fsin(angle: integer): integer; external;
function  psx_fcos(angle: integer): integer; external;

{ Textures by integer handle. A Pascal program cannot pass the TIM pointer, so
  it loads a texture through a small C accessor that calls psx_tex_upload and
  returns the handle (see examples/psx/sprite_asset.c), then draws by handle. }
procedure psx_tex_draw(tex, x, y: integer); external;
procedure psx_tex_draw_uv(tex, x, y, u, v, w, h: integer); external;
procedure psx_tex_rot(tex, cx, cy, angle, scale: integer); external;

{ Sound and CD-ROM bring-up (mounting the disc takes a path string, a C call). }
procedure psx_spu_init; external;
procedure psx_spu_play(voice, addr, pitch, vol: integer); external;
procedure psx_spu_stop(voice: integer); external;
procedure psx_cd_init; external;

{ 3D through the GTE, using the pointer-free helpers (libpsx/gte_easy.c), so a
  Pascal program needs no matrix or vertex arrays. Set the view once, build a
  rotation from three angles, project each vertex, and read its screen x, y, and
  depth back. See examples/psx/cube.pas. }
procedure psx_gte_set_screen(h, ofx, ofy: integer); external;
procedure psx_gte_set_trans(tx, ty, tz: integer); external;
procedure psx_gte_rot_xyz(ax, ay, az: integer); external;
procedure psx_gte_project(vx, vy, vz: integer); external;
function  psx_gte_sx: integer; external;
function  psx_gte_sy: integer; external;
function  psx_gte_sz: integer; external;

{ Ordering table: depth-sorted drawing. Clear it, add each face at its depth
  bucket (from the projected vertices' average depth), link it, then swap. }
procedure psx_ot_clear; external;
procedure psx_ot_add_poly4(x0, y0, x1, y1, x2, y2, x3, y3,
                           r, g, b, z: integer); external;
procedure psx_ot_add_gouraud4(x0, y0, c0, x1, y1, c1,
                              x2, y2, c2, x3, y3, c3, z: integer); external;
procedure psx_ot_link; external;
