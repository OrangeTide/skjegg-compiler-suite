{ hello.pas - a milestone-5 libpsx sample in Compact Pascal.

  The same bouncing square as the C hello_gpu, written in Pascal, to show the
  Pascal front end reaching the PlayStation. It calls libpsx through external
  declarations (the thin psx unit of milestone 6 formalizes these). Pascal and
  libpsx share the toolkit stack calling convention, so the calls bind with no
  glue. It uses no Pascal runtime features, so it links against start_psx and
  libpsx alone. }

program hellopsx;

procedure psx_gpu_init; external;
procedure psx_irq_init; external;
procedure psx_set_bg(r, g, b: integer); external;
procedure psx_draw_rect(x, y, w, h, r, g, b: integer); external;
procedure psx_vsync; external;
procedure psx_display_swap; external;

var
  f, x, dx: integer;

begin
  psx_gpu_init;
  psx_irq_init;
  psx_set_bg(0, 0, 40);
  x := 0;
  dx := 2;
  for f := 0 to 179 do
  begin
    psx_draw_rect(x, 104, 32, 32, 255, 200, 0);
    psx_vsync;
    psx_display_swap;
    x := x + dx;
    if (x <= 0) or (x >= 288) then
      dx := -dx;
  end;
end.
