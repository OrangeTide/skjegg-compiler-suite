{ cube.pas - a spinning 3D cube in Compact Pascal through the psx unit's GTE
  helpers.

  The Pascal counterpart of cube.c. Compact Pascal cannot pass the matrix and
  vertex arrays the raw GTE calls take, so it uses the pointer-free helpers
  (libpsx/gte_easy.c): build a rotation from two angles, project each cube corner,
  and read its screen position and depth back as plain integers. The six faces go
  into the ordering table at their average depth, so a single swap draws them back
  to front. All faces are submitted (no backface cull), which the painter's order
  of the table resolves for a convex cube.

  Compact Pascal has no unit system, so the bindings are declared inline here;
  units/psx.pas is the canonical copy. }

program cubepas;

procedure psx_gpu_init; external;
procedure psx_irq_init; external;
procedure psx_set_bg(r, g, b: integer); external;
procedure psx_vsync; external;
procedure psx_display_swap; external;
procedure psx_gte_set_screen(h, ofx, ofy: integer); external;
procedure psx_gte_set_trans(tx, ty, tz: integer); external;
procedure psx_gte_rot_xyz(ax, ay, az: integer); external;
procedure psx_gte_project(vx, vy, vz: integer); external;
function  psx_gte_sx: integer; external;
function  psx_gte_sy: integer; external;
function  psx_gte_sz: integer; external;
procedure psx_ot_clear; external;
procedure psx_ot_add_poly4(x0, y0, x1, y1, x2, y2, x3, y3,
                           r, g, b, z: integer); external;
procedure psx_ot_link; external;

const
  { Eight cube corners, and the six faces as four corner indices each, wound so
    the quad's GPU order is corner 0, 1, 3, 2. Face colors follow. }
  V: array[0..7, 0..2] of integer = (
    (-40, -40, -40), ( 40, -40, -40), ( 40,  40, -40), (-40,  40, -40),
    (-40, -40,  40), ( 40, -40,  40), ( 40,  40,  40), (-40,  40,  40));
  F: array[0..5, 0..3] of integer = (
    (4, 5, 6, 7), (1, 0, 3, 2), (0, 4, 7, 3),
    (5, 1, 2, 6), (0, 1, 5, 4), (3, 7, 6, 2));
  CR: array[0..5] of integer = (255, 0, 0, 255, 255, 0);
  CG: array[0..5] of integer = (0, 255, 0, 255, 0, 255);
  CB: array[0..5] of integer = (0, 0, 255, 0, 255, 255);

var
  frame, fi, angle: integer;
  ia, ib, ic, id, z: integer;
  x0, y0, x1, y1, x2, y2, x3, y3: integer;

begin
  psx_gpu_init;
  psx_irq_init;
  psx_set_bg(0, 0, 20);
  psx_gte_set_screen(300, 160, 120);
  angle := 0;

  for frame := 0 to 179 do
  begin
    psx_gte_rot_xyz(angle div 2, angle, 0);
    psx_gte_set_trans(0, 0, 220);
    psx_ot_clear;

    for fi := 0 to 5 do
    begin
      ia := F[fi, 0];
      ib := F[fi, 1];
      ic := F[fi, 2];
      id := F[fi, 3];

      { Quad vertex order is a, b, d, c (two triangles share the b-c edge). }
      psx_gte_project(V[ia, 0], V[ia, 1], V[ia, 2]);
      x0 := psx_gte_sx; y0 := psx_gte_sy; z := psx_gte_sz;
      psx_gte_project(V[ib, 0], V[ib, 1], V[ib, 2]);
      x1 := psx_gte_sx; y1 := psx_gte_sy; z := z + psx_gte_sz;
      psx_gte_project(V[id, 0], V[id, 1], V[id, 2]);
      x2 := psx_gte_sx; y2 := psx_gte_sy; z := z + psx_gte_sz;
      psx_gte_project(V[ic, 0], V[ic, 1], V[ic, 2]);
      x3 := psx_gte_sx; y3 := psx_gte_sy; z := z + psx_gte_sz;

      psx_ot_add_poly4(x0, y0, x1, y1, x2, y2, x3, y3,
                       CR[fi], CG[fi], CB[fi], z div 4);
    end;

    psx_ot_link;
    psx_vsync;
    psx_display_swap;
    angle := (angle + 4) and 1023;
  end;
end.
