unit raylib;
{ A thin Kobold binding for raylib: a window, immediate-mode 2D drawing, a few
  color and vector helpers. It is an optional native-backed unit, built only when
  ./configure found raylib (HAVE_RAYLIB); its C glue (kobold/lib/raylib_glue.c)
  is archived inside raylib.a and the link flags ride in the raylib.link sidecar.
  See doc/languages/kobold/kp-native-units.md.

  Unlike the sdl3 unit, which crosses its structs by reference, this binding uses
  the by-value record FFI, because that is raylib's own C ABI: a Color, a Vector2,
  and a Rectangle pass in registers by value, and GetColor and Vector2Add return a
  record by value. Color's fields are the sub-word `byte` type, so the record has
  raylib's Color layout (four 8-bit channels); Vector2 and Rectangle are `single`
  floats. raylib keeps a single global window, so there is no window id; the glue
  owns the lifetime. This is AOT only; the JIT cannot reach an external library's
  symbols.

  The routines keep raylib's own names (InitWindow, DrawRectangle). A Kobold unit
  compiles its public routines to symbols qualified by the unit name (raylib.X),
  so they do not collide with raylib's bare C symbols at link time even though the
  names match. See doc/languages/kobold/kp-native-units.md. }
interface

type
  { raylib Color: four 8-bit channels, the record's four `byte` fields at offsets
    0..3, four bytes total. Crosses by value as one integer register. }
  Color = record r: byte; g: byte; b: byte; a: byte; end;

  { raylib Vector2: two floats, eight bytes. Crosses by value in an SSE register. }
  Vector2 = record x: single; y: single; end;

  { raylib Rectangle: four floats, sixteen bytes. Crosses by value in two SSE
    registers. }
  Rectangle = record x: single; y: single; width: single; height: single; end;

const
  { TraceLogLevel values, for SetTraceLogLevel. }
  LOG_ALL     = 0;
  LOG_TRACE   = 1;
  LOG_DEBUG   = 2;
  LOG_INFO    = 3;
  LOG_WARNING = 4;
  LOG_ERROR   = 5;
  LOG_NONE    = 7;

{ Window lifetime. raylib has one global window. }
procedure InitWindow(w, h: integer; const title: string);
procedure CloseWindow;
function IsWindowReady: boolean;
function WindowShouldClose: boolean;
function GetScreenWidth: integer;
function GetScreenHeight: integer;
procedure SetTargetFPS(fps: integer);
procedure SetTraceLogLevel(level: integer);

{ A frame is BeginDrawing, some draw calls, then EndDrawing. }
procedure BeginDrawing;
procedure EndDrawing;
procedure ClearBackground(c: Color);
procedure DrawRectangle(x, y, w, h: integer; c: Color);
procedure DrawRectangleRec(const rec: Rectangle; c: Color);
procedure DrawCircle(cx, cy: integer; radius: single; c: Color);
procedure DrawText(const txt: string; x, y, fontSize: integer; c: Color);

{ Color helpers, exercising a record returned by value and taken by value. }
function ColorToInt(c: Color): integer;         { pack to 0xRRGGBBAA }
function GetColor(hexValue: integer): Color;     { unpack a 0xRRGGBBAA pixel }

{ A little raymath, wrapped here because raymath.h is header-only (inline). }
function Vector2Add(a, b: Vector2): Vector2;
function Vector2Length(v: Vector2): single;

implementation

{ The C glue (kobold/lib/raylib_glue.c), declared once here; resolved at link from
  the object archived in raylib.a, with the raylib link flags from the .link
  sidecar. The glue entry points are kpray_*, distinct from raylib's own symbols;
  the public wrappers below carry raylib's names, emitted as raylib.<name>. A plain
  record parameter crosses by value, matching raylib's C ABI. }
procedure kpray_init_window(w, h: integer; const title: string); cdecl; external;
procedure kpray_close_window; cdecl; external;
function kpray_window_ready: integer; cdecl; external;
function kpray_window_should_close: integer; cdecl; external;
function kpray_screen_width: integer; cdecl; external;
function kpray_screen_height: integer; cdecl; external;
procedure kpray_set_target_fps(fps: integer); cdecl; external;
procedure kpray_set_trace_log(level: integer); cdecl; external;
procedure kpray_begin_drawing; cdecl; external;
procedure kpray_end_drawing; cdecl; external;
procedure kpray_clear_background(c: Color); cdecl; external;
procedure kpray_draw_rectangle(x, y, w, h: integer; c: Color); cdecl; external;
procedure kpray_draw_rectangle_rec(const rec: Rectangle; c: Color); cdecl; external;
procedure kpray_draw_circle(cx, cy: integer; radius: single; c: Color); cdecl; external;
procedure kpray_draw_text(const txt: string; x, y, fontSize: integer; c: Color); cdecl; external;
function kpray_color_to_int(c: Color): integer; cdecl; external;
function kpray_get_color(hexValue: integer): Color; cdecl; external;
function kpray_vec2_add(a, b: Vector2): Vector2; cdecl; external;
function kpray_vec2_length(v: Vector2): single; cdecl; external;

procedure InitWindow(w, h: integer; const title: string);
begin
  kpray_init_window(w, h, title);
end;

procedure CloseWindow;
begin
  kpray_close_window;
end;

function IsWindowReady: boolean;
begin
  IsWindowReady := kpray_window_ready <> 0;
end;

function WindowShouldClose: boolean;
begin
  WindowShouldClose := kpray_window_should_close <> 0;
end;

function GetScreenWidth: integer;
begin
  GetScreenWidth := kpray_screen_width;
end;

function GetScreenHeight: integer;
begin
  GetScreenHeight := kpray_screen_height;
end;

procedure SetTargetFPS(fps: integer);
begin
  kpray_set_target_fps(fps);
end;

procedure SetTraceLogLevel(level: integer);
begin
  kpray_set_trace_log(level);
end;

procedure BeginDrawing;
begin
  kpray_begin_drawing;
end;

procedure EndDrawing;
begin
  kpray_end_drawing;
end;

procedure ClearBackground(c: Color);
begin
  kpray_clear_background(c);
end;

procedure DrawRectangle(x, y, w, h: integer; c: Color);
begin
  kpray_draw_rectangle(x, y, w, h, c);
end;

procedure DrawRectangleRec(const rec: Rectangle; c: Color);
begin
  kpray_draw_rectangle_rec(rec, c);
end;

procedure DrawCircle(cx, cy: integer; radius: single; c: Color);
begin
  kpray_draw_circle(cx, cy, radius, c);
end;

procedure DrawText(const txt: string; x, y, fontSize: integer; c: Color);
begin
  kpray_draw_text(txt, x, y, fontSize, c);
end;

function ColorToInt(c: Color): integer;
begin
  ColorToInt := kpray_color_to_int(c);
end;

function GetColor(hexValue: integer): Color;
begin
  GetColor := kpray_get_color(hexValue);
end;

function Vector2Add(a, b: Vector2): Vector2;
begin
  Vector2Add := kpray_vec2_add(a, b);
end;

function Vector2Length(v: Vector2): single;
begin
  Vector2Length := kpray_vec2_length(v);
end;

end.
