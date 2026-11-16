/* C glue for the Kobold struct-FFI suite.  Every record crosses the boundary as a
   pointer: a Kobold `var`/`const` record argument is a C `T*` / `const T*`.  A
   Kobold `integer` field is a 4-byte C int, `int64` an 8-byte long long, `single`
   a 4-byte float; the C struct uses the matching widths. */

#include <stdint.h>

struct Point { int x; int y; };
struct FRect { float x; float y; float w; float h; };

/* Padding differs between a natural and a packed struct of {int; int64}: natural
   puts the int64 at offset 8 (4 bytes of padding), packed at offset 4.  Writing a
   distinct 64-bit value lets the Kobold reader prove which offset it used. */
struct Nat { int a; int64_t b; };
struct __attribute__((packed)) Pak { int a; int64_t b; };

int
bump(struct Point *p)
{
    int sx = p->x;
    int sy = p->y;
    p->x = sx + sy;     /* 7 */
    p->y = sx * sy;     /* 12 */
    return sx + sy;     /* 7 */
}

float
area(const struct FRect *r)
{
    return r->w * r->h; /* 12.0 */
}

void
fillnat(struct Nat *r)
{
    r->a = 11;
    r->b = 2200000000LL;
}

void
fillpak(struct Pak *r)
{
    r->a = 11;
    r->b = 2200000000LL;
}

/* Sub-word integer fields: a Kobold `byte`/`word`/`shortint`/`smallint` record field
   is 1 or 2 bytes, matching a C uint8/uint16/int8/int16, with the same alignment and
   sign extension. */
#include <stdint.h>
struct Color { uint8_t r; uint8_t g; uint8_t b; uint8_t a; };
struct Sw { uint8_t tag; uint16_t half; int16_t sx; int32_t n; };

void
setcolor(struct Color *c)
{
    c->r = 10;
    c->g = 20;
    c->b = 30;
    c->a = 255;
}

void
setsw(struct Sw *s)
{
    s->tag = 7;
    s->half = 60000;   /* > 32767: proves unsigned 16-bit */
    s->sx = -1000;     /* negative: proves signed 16-bit sign extension */
    s->n = 123456;
}

/* By-value small-struct FFI: a cdecl callee takes an all-integer record of up to 8
   bytes in a general register (SysV rax/rdi, AAPCS64 x0), and returns one the same
   way. These exercise the caller passing and reading such a record by value. */
struct BvP2 { int x; int y; };          /* 8 bytes: one INTEGER eightbyte */
struct BvC4 { unsigned char r, g, b, a; }; /* 4 bytes */

int
bv_sum2(struct BvP2 p)                   /* x in low 32, y in high 32 of the reg */
{
    return p.x + p.y;
}

int
bv_sumc(struct BvC4 c)                   /* four bytes packed in the low word */
{
    return c.r + c.g + c.b + c.a;
}

struct BvP2
bv_mk2(int a, int b)                     /* returned by value in the integer register */
{
    struct BvP2 p;
    p.x = a;
    p.y = b;
    return p;
}

int
bv_mix(int n, struct BvP2 p)             /* a struct after an int: next registers */
{
    return n + p.x + p.y;
}

int
bv_over(int a, int b, int c, int d, int e, int f, int g, int h, struct BvP2 p)
{
    /* eight integer arguments exhaust the integer argument registers, so the
       by-value struct that follows is passed on the stack (the ARG_MEM / all-or-
       nothing path on x86-64 and arm64; a natural stack placement on rv32). */
    return a + b + c + d + e + f + g + h + p.x + p.y;
}

/* By-value floating-point small-struct FFI (Phase 2): a record of floats crosses by
   value in floating-point registers.  On x86-64 (SysV) a float eightbyte rides an SSE
   register (xmm); on arm64 (AAPCS64) a homogeneous float aggregate rides the s/d
   registers.  These exercise the caller decomposing such a record into its FP slots and
   reading an FP-register struct result.  (Skipped on rv32, whose soft-float ilp32 ABI
   passes a struct's words in integer registers while the zig hard-float glue would read
   them from FP registers.) */
struct BvFColor { float r, g, b, a; };  /* 16 bytes: two SSE eightbytes / a 4-float HFA */
struct BvFPoint { float x, y; };        /* 8 bytes: one SSE eightbyte / a 2-float HFA */
struct BvFMix { int n; float f; };      /* 8 bytes: one INTEGER eightbyte (int + float) */
struct BvDPair { double a, b; };        /* 16 bytes: two SSE eightbytes / a 2-double HFA */

float
bv_fcolor(struct BvFColor c)
{
    return c.r + c.g + c.b + c.a;
}

float
bv_fpoint(struct BvFPoint p)
{
    return p.x + p.y;
}

int
bv_fmix(struct BvFMix m)                 /* the eightbyte is INTEGER: it rides a gp reg */
{
    return m.n + (int)m.f;
}

double
bv_dpair(struct BvDPair d)
{
    return d.a + d.b;
}

struct BvFPoint
bv_mkfp(int x, int y)                    /* an FP-register struct returned by value */
{
    struct BvFPoint p;
    p.x = (float)x;
    p.y = (float)y;
    return p;
}

struct BvFColor
bv_mkcolor(int r, int g, int b, int a)   /* a 16-byte FP struct returned by value */
{
    struct BvFColor c;
    c.r = (float)r;
    c.g = (float)g;
    c.b = (float)b;
    c.a = (float)a;
    return c;
}

struct BvDPair
bv_mkdpair(int a, int b)                 /* a double HFA / two SSE eightbytes by value */
{
    struct BvDPair d;
    d.a = (double)a;
    d.b = (double)b;
    return d;
}

/* ---- Phase 3: the callee side.  C calls a Kobold `cdecl` routine that itself takes or
   returns a small record by value, so the Kobold routine reconstructs the record from the
   argument registers and scatters its result into the return registers.  Each trampoline
   receives the Kobold routine as a C function pointer, calls it with a by-value record (or
   reads a by-value record back), and returns a scalar the Kobold program prints. */
int
cb_call_p2(int (*f)(struct Point))       /* one INTEGER eightbyte / two rv32 words */
{
    struct Point p = { 3, 4 };
    return f(p);                         /* 7 */
}

int
cb_call_mkp2(struct Point (*f)(int, int))  /* Kobold returns the record in registers */
{
    struct Point p = f(10, 20);
    return p.x + p.y;                    /* 30 */
}

float
cb_call_fp(float (*f)(struct BvFPoint))  /* one SSE eightbyte / a 2-float HFA */
{
    struct BvFPoint p = { 3.0f, 4.0f };
    return f(p);                         /* 7 */
}

float
cb_call_mkfp(struct BvFPoint (*f)(int, int))
{
    struct BvFPoint p = f(1, 2);
    return p.x + p.y;                    /* 3 */
}

float
cb_call_fc(float (*f)(struct BvFColor))  /* two SSE eightbytes / a 4-float HFA */
{
    struct BvFColor c = { 10.0f, 20.0f, 30.0f, 40.0f };
    return f(c);                         /* 100 */
}

float
cb_call_mkfc(struct BvFColor (*f)(int, int, int, int))
{
    struct BvFColor c = f(1, 2, 3, 4);
    return c.r + c.g + c.b + c.a;        /* 10 */
}
