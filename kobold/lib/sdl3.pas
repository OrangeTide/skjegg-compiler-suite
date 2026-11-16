unit sdl3;
{ A thin Kobold binding for SDL3: window creation, 2D rendering, and polled
  input. It is an optional native-backed unit, built only when ./configure found
  SDL3 (HAVE_SDL3); its C glue (kobold/lib/sdl3_glue.c) is archived inside sdl3.a
  and the -lSDL3 link flags ride in the sdl3.link sidecar. See
  doc/languages/kobold/kp-native-units.md.

  The SDL structs are Kobold records that cross the FFI by reference, so the API
  reads the way the C one does: a color is a Color, a rectangle a Rect or FRect,
  an event a 128-byte buffer filled in place by PollEvent. Color's fields are the
  sub-word `byte` type, so the record has SDL_Color's exact four-byte layout.

  SDL windows and renderers are named by an integer id, never a pointer: the glue
  keeps the real pointers in a bounded table, so CreateWindow and CreateRenderer
  return an id > 0 (0 on failure) that later calls pass back. Input is polled, not
  delivered through a callback. This is AOT only; the JIT cannot reach an external
  library's symbols. }
interface

type
  { SDL_Color: four 8-bit channels, the record's four `byte` fields at offsets
    0..3. }
  Color = record r: byte; g: byte; b: byte; a: byte; end;

  { SDL_Rect / SDL_FRect: an integer and a float rectangle. SDL3's renderer draws
    in float coordinates (FRect); the integer Rect serves the geometry helpers. }
  Rect  = record x: integer; y: integer; w: integer; h: integer; end;
  FRect = record x: single; y: single; w: single; h: single; end;

  { SDL_Event is a 128-byte union. PollEvent fills this buffer; the Event*
    accessors read the active member's fields (the glue knows the union layout). }
  Event = record raw: array[0..31] of integer; end;

const
  { EventType results, a small normalized enum (see sdl3_glue.c). }
  EvNone        = 0;
  EvQuit        = 1;
  EvKeyDown     = 2;
  EvKeyUp       = 3;
  EvMouseMove   = 4;
  EvMouseButton = 5;
  EvOther       = 6;

{ Subsystem lifetime. }
function InitVideo: boolean;                  { start the video subsystem; false on failure }
procedure Quit;                               { shut SDL down }

{ Windows and renderers, named by id (> 0 on success, 0 on failure). }
function CreateWindow(const title: string; w, h: integer): integer;
procedure DestroyWindow(win: integer);
procedure GetWindowSize(win: integer; var w: integer; var h: integer);
function GetWindowTitle(win: integer): string;    { the title set at creation }
function CreateRenderer(win: integer): integer;
procedure DestroyRenderer(ren: integer);

{ 2D drawing. Each returns false if the renderer id is invalid or SDL failed. }
function SetDrawColor(ren: integer; const c: Color): boolean;
function Clear(ren: integer): boolean;
function FillRect(ren: integer; const r: FRect): boolean;
procedure Present(ren: integer);

{ Pure geometry, record in and out: the intersection of two rectangles. False
  (and an untouched out) when they do not overlap. }
function RectIntersect(const a: Rect; const b: Rect; var out: Rect): boolean;
function RectIntersectF(const a: FRect; const b: FRect; var out: FRect): boolean;

{ Pack a Color into a 32-bit RGBA8888 pixel. }
function MapRGBA(const c: Color): cardinal;

{ Polled input: PollEvent fills ev and returns false when the queue is empty; the
  accessors read the filled event. }
function PollEvent(var ev: Event): boolean;
function EventType(const ev: Event): integer;  { one of the Ev* constants }
function EventKey(const ev: Event): integer;    { keycode, for EvKeyDown / EvKeyUp }
function EventMouseX(const ev: Event): integer;
function EventMouseY(const ev: Event): integer;

procedure Delay(ms: integer);                   { sleep, in milliseconds }

implementation

{ The C glue (kobold/lib/sdl3_glue.c), declared once here; resolved at link from
  the object archived in sdl3.a, with -lSDL3 from the unit's .link sidecar. A
  record parameter crosses by reference, so each `const`/`var` record below is a
  C pointer to the matching SDL struct. }
function kpsdl_init_video: integer; cdecl; external;
procedure kpsdl_quit; cdecl; external;
function kpsdl_create_window(const title: string; w, h: integer): integer; cdecl; external;
procedure kpsdl_destroy_window(id: integer); cdecl; external;
procedure kpsdl_window_size(id: integer; var w: integer; var h: integer); cdecl; external;
function kpsdl_window_title(id: integer): string; cdecl; external;
function kpsdl_create_renderer(win: integer): integer; cdecl; external;
procedure kpsdl_destroy_renderer(id: integer); cdecl; external;
function kpsdl_set_draw_color(ren: integer; const c: Color): integer; cdecl; external;
function kpsdl_clear(ren: integer): integer; cdecl; external;
function kpsdl_fill_rect(ren: integer; const r: FRect): integer; cdecl; external;
procedure kpsdl_present(ren: integer); cdecl; external;
function kpsdl_rect_isect(const a: Rect; const b: Rect; var out: Rect): integer; cdecl; external;
function kpsdl_rect_isect_f(const a: FRect; const b: FRect; var out: FRect): integer; cdecl; external;
function kpsdl_map_rgba(const c: Color): cardinal; cdecl; external;
function kpsdl_poll(var ev: Event): integer; cdecl; external;
function kpsdl_event_type(const ev: Event): integer; cdecl; external;
function kpsdl_event_key(const ev: Event): integer; cdecl; external;
function kpsdl_event_mouse_x(const ev: Event): integer; cdecl; external;
function kpsdl_event_mouse_y(const ev: Event): integer; cdecl; external;
procedure kpsdl_delay(ms: integer); cdecl; external;

function InitVideo: boolean;
begin
  InitVideo := kpsdl_init_video <> 0;
end;

procedure Quit;
begin
  kpsdl_quit;
end;

function CreateWindow(const title: string; w, h: integer): integer;
begin
  CreateWindow := kpsdl_create_window(title, w, h);
end;

procedure DestroyWindow(win: integer);
begin
  kpsdl_destroy_window(win);
end;

procedure GetWindowSize(win: integer; var w: integer; var h: integer);
begin
  kpsdl_window_size(win, w, h);
end;

function GetWindowTitle(win: integer): string;
begin
  GetWindowTitle := kpsdl_window_title(win);
end;

function CreateRenderer(win: integer): integer;
begin
  CreateRenderer := kpsdl_create_renderer(win);
end;

procedure DestroyRenderer(ren: integer);
begin
  kpsdl_destroy_renderer(ren);
end;

function SetDrawColor(ren: integer; const c: Color): boolean;
begin
  SetDrawColor := kpsdl_set_draw_color(ren, c) <> 0;
end;

function Clear(ren: integer): boolean;
begin
  Clear := kpsdl_clear(ren) <> 0;
end;

function FillRect(ren: integer; const r: FRect): boolean;
begin
  FillRect := kpsdl_fill_rect(ren, r) <> 0;
end;

procedure Present(ren: integer);
begin
  kpsdl_present(ren);
end;

function RectIntersect(const a: Rect; const b: Rect; var out: Rect): boolean;
begin
  RectIntersect := kpsdl_rect_isect(a, b, out) <> 0;
end;

function RectIntersectF(const a: FRect; const b: FRect; var out: FRect): boolean;
begin
  RectIntersectF := kpsdl_rect_isect_f(a, b, out) <> 0;
end;

function MapRGBA(const c: Color): cardinal;
begin
  MapRGBA := kpsdl_map_rgba(c);
end;

function PollEvent(var ev: Event): boolean;
begin
  PollEvent := kpsdl_poll(ev) <> 0;
end;

function EventType(const ev: Event): integer;
begin
  EventType := kpsdl_event_type(ev);
end;

function EventKey(const ev: Event): integer;
begin
  EventKey := kpsdl_event_key(ev);
end;

function EventMouseX(const ev: Event): integer;
begin
  EventMouseX := kpsdl_event_mouse_x(ev);
end;

function EventMouseY(const ev: Event): integer;
begin
  EventMouseY := kpsdl_event_mouse_y(ev);
end;

procedure Delay(ms: integer);
begin
  kpsdl_delay(ms);
end;

end.
