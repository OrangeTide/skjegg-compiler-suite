/* C glue for the optional `sdl3` unit: a thin binding to SDL3. Compiled with the
   library's own cflags and archived inside sdl3.a; the -lSDL3 link flags ride in
   the sdl3.link sidecar (see doc/languages/kobold/kp-native-units.md).

   The Kobold records (Color, Rect, FRect, Event) have the layout of the matching
   SDL structs, so each crosses by reference and this glue forwards the pointer to
   SDL unchanged. Where an SDL entry point takes a struct by value (the color
   setters take four Uint8), the glue reads the record's fields and makes the
   by-value call. Kobold holds no pointers, so a window or renderer is a small
   integer id into a bounded table, never an address.

   A Kobold `string` (the window title) crosses as a guest string handle (kp_gptr),
   not a C `char *`; the glue reads its bytes with kp_str_bytes (str.h), and is
   compiled -DKP_FREESTANDING to match libkpaot's no-vm signature. See the "Strings
   cross as a handle" section of doc/languages/kobold/kp-native-units.md. */
#include <SDL3/SDL.h>
#include <string.h>

#include "str.h"        /* kp_gptr, kp_str_bytes / kp_str_new: the guest-string sides */

/* Normalized event kinds, matching the Ev* constants in sdl3.pas. */
#define KPSDL_EV_NONE        0
#define KPSDL_EV_QUIT        1
#define KPSDL_EV_KEYDOWN     2
#define KPSDL_EV_KEYUP       3
#define KPSDL_EV_MOUSEMOVE   4
#define KPSDL_EV_MOUSEBUTTON 5
#define KPSDL_EV_OTHER       6

/* --- subsystem lifetime --- */

int
kpsdl_init_video(void)
{
    return SDL_Init(SDL_INIT_VIDEO) ? 1 : 0;
}

void
kpsdl_quit(void)
{
    SDL_Quit();
}

/* --- windows and renderers, named by a 1-based integer id --- */

#define KPSDL_MAX_WINDOW   16
#define KPSDL_MAX_RENDERER 16

static SDL_Window   *g_window[KPSDL_MAX_WINDOW];
static SDL_Renderer *g_renderer[KPSDL_MAX_RENDERER];

static SDL_Window *
window_of(int id)
{
    return (id >= 1 && id <= KPSDL_MAX_WINDOW) ? g_window[id - 1] : NULL;
}

static SDL_Renderer *
renderer_of(int id)
{
    return (id >= 1 && id <= KPSDL_MAX_RENDERER) ? g_renderer[id - 1] : NULL;
}

int
kpsdl_create_window(kp_gptr title, int w, int h)
{
    int i;

    for (i = 0; i < KPSDL_MAX_WINDOW; i++)
        if (g_window[i] == NULL)
            break;
    if (i == KPSDL_MAX_WINDOW)
        return 0;                       /* table full */
    g_window[i] = SDL_CreateWindow(kp_str_bytes(title), w, h, 0);
    return g_window[i] ? i + 1 : 0;     /* 1-based id, 0 on failure */
}

void
kpsdl_destroy_window(int id)
{
    SDL_Window *win = window_of(id);

    if (win) {
        SDL_DestroyWindow(win);
        g_window[id - 1] = NULL;
    }
}

void
kpsdl_window_size(int id, int *w, int *h)
{
    SDL_Window *win = window_of(id);

    *w = 0;
    *h = 0;
    if (win)
        SDL_GetWindowSize(win, w, h);
}

/* The window's title as a new Kobold string, the reverse of the handle direction:
   the glue builds the guest string with kp_str_new and returns its handle. Empty
   for an invalid id. */
kp_gptr
kpsdl_window_title(int id)
{
    SDL_Window *win = window_of(id);
    const char *t = win ? SDL_GetWindowTitle(win) : "";

    if (t == NULL)
        t = "";
    return kp_str_new(t, (int)strlen(t));
}

int
kpsdl_create_renderer(int winid)
{
    SDL_Window *win = window_of(winid);
    int i;

    if (!win)
        return 0;
    for (i = 0; i < KPSDL_MAX_RENDERER; i++)
        if (g_renderer[i] == NULL)
            break;
    if (i == KPSDL_MAX_RENDERER)
        return 0;
    g_renderer[i] = SDL_CreateRenderer(win, NULL);
    return g_renderer[i] ? i + 1 : 0;
}

void
kpsdl_destroy_renderer(int id)
{
    SDL_Renderer *ren = renderer_of(id);

    if (ren) {
        SDL_DestroyRenderer(ren);
        g_renderer[id - 1] = NULL;
    }
}

/* --- 2D drawing: records by reference, by-value SDL calls bridged here --- */

int
kpsdl_set_draw_color(int renid, const SDL_Color *c)
{
    SDL_Renderer *ren = renderer_of(renid);

    if (!ren)
        return 0;
    /* SDL takes the four channels by value; the glue reads the Color record. */
    return SDL_SetRenderDrawColor(ren, c->r, c->g, c->b, c->a) ? 1 : 0;
}

int
kpsdl_clear(int renid)
{
    SDL_Renderer *ren = renderer_of(renid);

    return (ren && SDL_RenderClear(ren)) ? 1 : 0;
}

int
kpsdl_fill_rect(int renid, const SDL_FRect *r)
{
    SDL_Renderer *ren = renderer_of(renid);

    return (ren && SDL_RenderFillRect(ren, r)) ? 1 : 0;
}

void
kpsdl_present(int renid)
{
    SDL_Renderer *ren = renderer_of(renid);

    if (ren)
        SDL_RenderPresent(ren);
}

/* --- pure geometry and pixel packing: the record is the SDL struct --- */

int
kpsdl_rect_isect(const SDL_Rect *a, const SDL_Rect *b, SDL_Rect *out)
{
    return SDL_GetRectIntersection(a, b, out) ? 1 : 0;
}

int
kpsdl_rect_isect_f(const SDL_FRect *a, const SDL_FRect *b, SDL_FRect *out)
{
    return SDL_GetRectIntersectionFloat(a, b, out) ? 1 : 0;
}

unsigned int
kpsdl_map_rgba(const SDL_Color *c)
{
    const SDL_PixelFormatDetails *f =
        SDL_GetPixelFormatDetails(SDL_PIXELFORMAT_RGBA8888);

    return SDL_MapRGBA(f, NULL, c->r, c->g, c->b, c->a);
}

/* --- polled input: the Event record is SDL_Event's 128-byte buffer --- */

int
kpsdl_poll(SDL_Event *ev)
{
    return SDL_PollEvent(ev) ? 1 : 0;
}

int
kpsdl_event_type(const SDL_Event *ev)
{
    switch (ev->type) {
    case SDL_EVENT_QUIT:              return KPSDL_EV_QUIT;
    case SDL_EVENT_KEY_DOWN:          return KPSDL_EV_KEYDOWN;
    case SDL_EVENT_KEY_UP:            return KPSDL_EV_KEYUP;
    case SDL_EVENT_MOUSE_MOTION:      return KPSDL_EV_MOUSEMOVE;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:   return KPSDL_EV_MOUSEBUTTON;
    default:                          return KPSDL_EV_OTHER;
    }
}

int
kpsdl_event_key(const SDL_Event *ev)
{
    return (int)ev->key.key;
}

int
kpsdl_event_mouse_x(const SDL_Event *ev)
{
    if (ev->type == SDL_EVENT_MOUSE_MOTION)
        return (int)ev->motion.x;
    return (int)ev->button.x;
}

int
kpsdl_event_mouse_y(const SDL_Event *ev)
{
    if (ev->type == SDL_EVENT_MOUSE_MOTION)
        return (int)ev->motion.y;
    return (int)ev->button.y;
}

/* --- timing --- */

void
kpsdl_delay(int ms)
{
    SDL_Delay((Uint32)(ms < 0 ? 0 : ms));
}
