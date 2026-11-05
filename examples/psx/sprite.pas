{ sprite.pas - the Compact Pascal texture sample.

  Compact Pascal has no unit system, so the libpsx bindings are declared inline
  (units/psx.pas is the canonical copy). It loads two textures by handle through
  the C accessor in sprite_asset.c (Pascal has no pointers, so the raw TIM
  pointer stays in C), draws one as a plain sprite and the other rotated, and
  runs a fixed number of frames. It proves Compact Pascal reaches the richer 2D
  draws, the same as the C and Kobold samples. }

program spritepas;

procedure psx_gpu_init; external;
procedure psx_irq_init; external;
procedure psx_set_bg(r, g, b: integer); external;
procedure psx_vsync; external;
procedure psx_display_swap; external;
procedure psx_tex_draw(tex, x, y: integer); external;
procedure psx_tex_rot(tex, cx, cy, angle, scale: integer); external;
function  psx_load_quad: integer; external;
function  psx_load_solid: integer; external;

const
  FixOne = 65536;

var
  f, quad, solid: integer;

begin
  psx_gpu_init;
  psx_irq_init;
  psx_set_bg(0, 0, 20);

  quad := psx_load_quad;
  solid := psx_load_solid;

  for f := 0 to 119 do
  begin
    psx_tex_draw(quad, 40, 40);
    psx_tex_rot(solid, 200, 120, 128, FixOne);
    psx_vsync;
    psx_display_swap;
  end;
end.
