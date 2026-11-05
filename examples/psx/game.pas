{ game.pas - the milestone-6 Compact Pascal sample: the coin-chase game.

  Mirrors the C game.c. Compact Pascal has no unit system, so the psx bindings
  are declared inline here (units/psx.pas is the canonical copy). Move the
  player with the d-pad onto the bobbing coin to score and beep; it runs a fixed
  number of frames. It uses no Pascal runtime, so it links freestanding against
  start_psx and libpsx. }

program gamepas;

procedure psx_gpu_init; external;
procedure psx_irq_init; external;
procedure psx_set_bg(r, g, b: integer); external;
procedure psx_draw_rect(x, y, w, h, r, g, b: integer); external;
procedure psx_vsync; external;
procedure psx_display_swap; external;
function  psx_pad_read: integer; external;
procedure psx_beep; external;
function  psx_fmul(a, b: integer): integer; external;
function  psx_fsin(angle: integer): integer; external;

const
  SZ = 20;
  FixOne = 65536;

var
  f, px, py, cx, cy, cby, score, b, rngv: integer;

function rng: integer;
begin
  rngv := rngv * 1103515245 + 12345;
  rng := (rngv div 65536) and 32767;
end;

function overlap(ax, ay, bx, by: integer): boolean;
begin
  overlap := (ax < bx + SZ) and (bx < ax + SZ) and
             (ay < by + SZ) and (by < ay + SZ);
end;

begin
  rngv := 1;
  psx_gpu_init;
  psx_irq_init;
  psx_set_bg(0, 0, 40);
  px := 150;
  py := 110;
  cx := 40;
  cy := 40;
  score := 0;

  for f := 0 to 899 do
  begin
    b := psx_pad_read;
    if (b and 128) <> 0 then px := px - 2;
    if (b and 32) <> 0 then px := px + 2;
    if (b and 16) <> 0 then py := py - 2;
    if (b and 64) <> 0 then py := py + 2;
    if px < 0 then px := 0;
    if px > 300 then px := 300;
    if py < 0 then py := 0;
    if py > 220 then py := 220;

    cby := cy + (psx_fmul(psx_fsin(f * 8), 6 * FixOne) div FixOne);

    if overlap(px, py, cx, cby) then
    begin
      score := score + 1;
      psx_beep;
      cx := rng mod 300;
      cy := (rng mod 208) + 6;
    end;

    psx_draw_rect(cx, cby, SZ, SZ, 0, 200, 255);
    psx_draw_rect(px, py, SZ, SZ, 255, 220, 0);
    psx_vsync;
    psx_display_swap;
  end;
end.
