unit stb_truetype;
{ A thin Kobold binding for stb_truetype, the public-domain single-header
  TrueType font loader and rasterizer. It is an optional native-backed unit; its C
  glue (kobold/lib/stb_truetype_glue.c) compiles the one stb_truetype
  implementation and is archived inside stb_truetype.a, with -lm in the
  stb_truetype.link sidecar. See doc/languages/kobold/kp-native-units.md.

  Two kinds of handle, both integer ids (> 0 on success, 0 on failure), since
  Kobold holds no pointers. LoadFont opens a .ttf and returns a font id; the glue
  keeps the file bytes alive for it. Render rasterizes one code point at a pixel
  height into a glyph id: an 8-bit coverage bitmap whose pixels Coverage reads,
  with GlyphWidth / GlyphHeight its size and GlyphXOff / GlyphYOff the offset of its
  top-left corner from the pen origin (y down, so a positive GlyphYOff is below the
  baseline-relative origin; typically negative for a capital). Vertical metrics and
  a code point's advance are reported scaled to the same pixel height. Release a
  glyph with FreeGlyph and the font with FreeFont. This is AOT only; the JIT cannot
  reach an external library's symbols. }
interface

{ Open a TrueType font file; a font id > 0, or 0 if the file is missing or is not a
  usable font. }
function LoadFont(const path: string): integer;
procedure FreeFont(font: integer);

{ Vertical metrics scaled to a cell `pixelHeight` pixels tall and rounded to whole
  pixels: the ascent above the baseline (positive), the descent below it
  (negative), and the recommended gap between lines. 0 for an invalid id. }
function Ascent(font, pixelHeight: integer): integer;
function Descent(font, pixelHeight: integer): integer;
function LineGap(font, pixelHeight: integer): integer;

{ The horizontal advance of `codepoint` at `pixelHeight`, in whole pixels. }
function Advance(font, codepoint, pixelHeight: integer): integer;

{ Rasterize `codepoint` at `pixelHeight` into a new glyph; a glyph id > 0, or 0 if
  the code point has no outline (a blank, such as the space) or the glyph table is
  full. }
function Render(font, codepoint, pixelHeight: integer): integer;
procedure FreeGlyph(glyph: integer);

{ The rendered glyph's bitmap size, and the offset of its top-left corner from the
  pen origin (x right, y down). 0 for an invalid id. }
function GlyphWidth(glyph: integer): integer;
function GlyphHeight(glyph: integer): integer;
function GlyphXOff(glyph: integer): integer;
function GlyphYOff(glyph: integer): integer;

{ The coverage (0..255) of the glyph bitmap at column x, row y (y = 0 the top row).
  0 for an invalid id or an out-of-range coordinate. }
function Coverage(glyph, x, y: integer): integer;

implementation

{ The C glue (kobold/lib/stb_truetype_glue.c), declared once here; resolved at link
  from the object archived in stb_truetype.a, with -lm from the unit's .link
  sidecar. }
function kpttf_load(const path: string): integer; cdecl; external;
procedure kpttf_free_font(id: integer); cdecl; external;
function kpttf_ascent(id, pixelHeight: integer): integer; cdecl; external;
function kpttf_descent(id, pixelHeight: integer): integer; cdecl; external;
function kpttf_line_gap(id, pixelHeight: integer): integer; cdecl; external;
function kpttf_advance(id, cp, pixelHeight: integer): integer; cdecl; external;
function kpttf_render(id, cp, pixelHeight: integer): integer; cdecl; external;
procedure kpttf_free_glyph(gid: integer); cdecl; external;
function kpttf_glyph_width(gid: integer): integer; cdecl; external;
function kpttf_glyph_height(gid: integer): integer; cdecl; external;
function kpttf_glyph_xoff(gid: integer): integer; cdecl; external;
function kpttf_glyph_yoff(gid: integer): integer; cdecl; external;
function kpttf_glyph_coverage(gid, x, y: integer): integer; cdecl; external;

function LoadFont(const path: string): integer;
begin
  LoadFont := kpttf_load(path);
end;

procedure FreeFont(font: integer);
begin
  kpttf_free_font(font);
end;

function Ascent(font, pixelHeight: integer): integer;
begin
  Ascent := kpttf_ascent(font, pixelHeight);
end;

function Descent(font, pixelHeight: integer): integer;
begin
  Descent := kpttf_descent(font, pixelHeight);
end;

function LineGap(font, pixelHeight: integer): integer;
begin
  LineGap := kpttf_line_gap(font, pixelHeight);
end;

function Advance(font, codepoint, pixelHeight: integer): integer;
begin
  Advance := kpttf_advance(font, codepoint, pixelHeight);
end;

function Render(font, codepoint, pixelHeight: integer): integer;
begin
  Render := kpttf_render(font, codepoint, pixelHeight);
end;

procedure FreeGlyph(glyph: integer);
begin
  kpttf_free_glyph(glyph);
end;

function GlyphWidth(glyph: integer): integer;
begin
  GlyphWidth := kpttf_glyph_width(glyph);
end;

function GlyphHeight(glyph: integer): integer;
begin
  GlyphHeight := kpttf_glyph_height(glyph);
end;

function GlyphXOff(glyph: integer): integer;
begin
  GlyphXOff := kpttf_glyph_xoff(glyph);
end;

function GlyphYOff(glyph: integer): integer;
begin
  GlyphYOff := kpttf_glyph_yoff(glyph);
end;

function Coverage(glyph, x, y: integer): integer;
begin
  Coverage := kpttf_glyph_coverage(glyph, x, y);
end;

end.
