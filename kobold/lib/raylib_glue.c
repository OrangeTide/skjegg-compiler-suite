/* C glue for the optional `raylib` unit: a thin binding to raylib. Compiled with
   the library's own cflags and archived inside raylib.a; the raylib link flags
   ride in the raylib.link sidecar (see doc/languages/kobold/kp-native-units.md).

   raylib's Color, Vector2, and Rectangle pass by value in registers, and GetColor
   and Vec2Add return one by value, so this is the worked example of the by-value
   record FFI rather than the by-reference crossing the sdl3 glue uses. The Kobold
   records have raylib's exact layout, so each value crosses straight through to
   raylib unchanged. raylib keeps one global window, so there is no handle table
   here; the glue only forwards.

   A Kobold `string` (the window title, the drawn text) crosses as a guest string
   handle (kp_gptr), not a C `char *`; the glue reads its bytes with kp_str_bytes
   (str.h), and is compiled -DKP_FREESTANDING to match libkpaot's no-vm signature.
   See the "Strings cross as a handle" section of
   doc/languages/kobold/kp-native-units.md. */
#include "raylib.h"
#include "raymath.h"

#include "str.h"        /* kp_gptr, kp_str_bytes: the guest-string read side */

/* --- window lifetime (one global window) --- */

void
kpray_init_window(int w, int h, kp_gptr title)
{
    /* Keep raylib quiet by default so the unit's stdout stays clean; a program
       can raise the level again through SetTraceLog. */
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(w, h, kp_str_bytes(title));
}

void
kpray_close_window(void)
{
    CloseWindow();
}

int
kpray_window_ready(void)
{
    return IsWindowReady() ? 1 : 0;
}

int
kpray_window_should_close(void)
{
    return WindowShouldClose() ? 1 : 0;
}

int
kpray_screen_width(void)
{
    return GetScreenWidth();
}

int
kpray_screen_height(void)
{
    return GetScreenHeight();
}

void
kpray_set_target_fps(int fps)
{
    SetTargetFPS(fps);
}

void
kpray_set_trace_log(int level)
{
    SetTraceLogLevel(level);
}

/* --- a frame --- */

void
kpray_begin_drawing(void)
{
    BeginDrawing();
}

void
kpray_end_drawing(void)
{
    EndDrawing();
}

void
kpray_clear_background(Color c)
{
    ClearBackground(c);
}

void
kpray_draw_rectangle(int x, int y, int w, int h, Color c)
{
    DrawRectangle(x, y, w, h, c);
}

void
kpray_draw_rectangle_rec(const Rectangle *r, Color c)
{
    /* The Rectangle is declared `const` in the unit, so it crosses by reference;
       raylib takes it by value, so dereference here. */
    DrawRectangleRec(*r, c);
}

void
kpray_draw_circle(int cx, int cy, float radius, Color c)
{
    DrawCircle(cx, cy, radius, c);
}

void
kpray_draw_text(kp_gptr text, int x, int y, int fontSize, Color c)
{
    DrawText(kp_str_bytes(text), x, y, fontSize, c);
}

/* --- color helpers: a record by value in, and returned by value --- */

int
kpray_color_to_int(Color c)
{
    return ColorToInt(c);
}

Color
kpray_get_color(int hexValue)
{
    return GetColor((unsigned int)hexValue);
}

/* --- a little raymath (header-only inline, so wrapped) --- */

Vector2
kpray_vec2_add(Vector2 a, Vector2 b)
{
    return Vector2Add(a, b);
}

float
kpray_vec2_length(Vector2 v)
{
    return Vector2Length(v);
}
