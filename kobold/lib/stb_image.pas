unit stb_image;
{ A thin Kobold binding for stb_image, the public-domain single-header image
  loader (PNG, JPEG, BMP, TGA, GIF, PSD, HDR, PIC, PNM). It is an optional
  native-backed unit; its C glue (kobold/lib/stb_image_glue.c) compiles the one
  stb_image implementation and is archived inside stb_image.a, with -lm in the
  stb_image.link sidecar. See doc/languages/kobold/kp-native-units.md.

  A decoded image is a heap buffer that stb returns as a pointer. Kobold holds no
  pointers, so Load keeps the buffer in the glue's table and returns an integer id
  (> 0 on success, 0 on failure) that the accessors pass back. Pixels are read one
  channel at a time through Pixel; the id is released with Free. This is AOT only;
  the JIT cannot reach an external library's symbols. }
interface

{ Decode an image file. Load keeps the file's own channel count (1 grey, 2
  grey+alpha, 3 RGB, 4 RGBA); LoadRGBA forces four channels (R, G, B, A),
  convenient for a texture. Each returns an id > 0, or 0 if the file is missing,
  is not a supported format, or the image table is full. }
function Load(const path: string): integer;
function LoadRGBA(const path: string): integer;

{ The decoded dimensions and the number of channels stored per pixel (what Pixel
  indexes: the file's own count after Load, always 4 after LoadRGBA). Each returns
  0 for an invalid id. }
function Width(img: integer): integer;
function Height(img: integer): integer;
function Channels(img: integer): integer;

{ One channel byte (0..255) of the pixel at column x, row y, with y = 0 the top
  row. Channel 0 is red (or grey), then green, blue, alpha. Returns 0 for an
  invalid id, an out-of-range coordinate, or a channel at or beyond the stored
  count. }
function Pixel(img, x, y, channel: integer): integer;

{ Release the image and its buffer. The id is invalid afterward. }
procedure Free(img: integer);

implementation

{ The C glue (kobold/lib/stb_image_glue.c), declared once here; resolved at link
  from the object archived in stb_image.a, with -lm from the unit's .link
  sidecar. }
function kpstbi_load(const path: string): integer; cdecl; external;
function kpstbi_load_rgba(const path: string): integer; cdecl; external;
function kpstbi_width(id: integer): integer; cdecl; external;
function kpstbi_height(id: integer): integer; cdecl; external;
function kpstbi_channels(id: integer): integer; cdecl; external;
function kpstbi_pixel(id, x, y, c: integer): integer; cdecl; external;
procedure kpstbi_free(id: integer); cdecl; external;

function Load(const path: string): integer;
begin
  Load := kpstbi_load(path);
end;

function LoadRGBA(const path: string): integer;
begin
  LoadRGBA := kpstbi_load_rgba(path);
end;

function Width(img: integer): integer;
begin
  Width := kpstbi_width(img);
end;

function Height(img: integer): integer;
begin
  Height := kpstbi_height(img);
end;

function Channels(img: integer): integer;
begin
  Channels := kpstbi_channels(img);
end;

function Pixel(img, x, y, channel: integer): integer;
begin
  Pixel := kpstbi_pixel(img, x, y, channel);
end;

procedure Free(img: integer);
begin
  kpstbi_free(img);
end;

end.
