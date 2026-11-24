/* pascal_rt.c : runtime helpers for Pascal (cross-compiled) */

#ifdef PAS_HOST
#include <unistd.h>
#include <stdio.h>
#include <math.h>
#else
extern int read(int fd, char *buf, int n);
extern int write(int fd, const char *buf, int n);
#endif

/****************************************************************
 * Heap strings (FPC-style ansistring; the Kobold string descriptor)
 *
 * A string value is a pointer to a {refcount, kind, len, cap} header (four
 * 32-bit words, 16 bytes) followed by the bytes and a trailing NUL.  The
 * pointer addresses the header; the bytes live at offset 16.  A NULL pointer
 * is the empty string.  A static literal carries refcount 0xffffffff and is
 * never freed.  The layout matches the backend's `strobj` global emission, so
 * a string literal is one of these objects sitting in rodata, endian-correct
 * on every target.
 *
 * Assignment shares a value by reference count (__pascal_str_retain /
 * __pascal_str_release) rather than copying; release frees at zero on the
 * host.  Allocation is libc malloc on the host and a static bump arena on the
 * freestanding targets, which run only small programs.
 ****************************************************************/

#ifdef PAS_HOST
#include <stdlib.h>
#define P_ALLOC(n) malloc((unsigned long)(n))
#define P_FREE(p)  free(p)
#else
static char p_arena[1u << 20];
static unsigned long p_arena_used;
static void *
P_ALLOC(unsigned long n)
{
    void *r;

    n = (n + 7u) & ~7u;
    if (p_arena_used + n > sizeof(p_arena))
        return 0;
    r = &p_arena[p_arena_used];
    p_arena_used += n;
    return r;
}
/* The bump arena has no free list; released storage is reclaimed only when
   the program exits.  The refcount still drops so logic stays uniform. */
#define P_FREE(p)  ((void)(p))
#endif

/* new(p) / dispose(p): the untyped heap behind Pascal pointers.  They share the
   string runtime's allocator, so a block is really reclaimed on the host and
   left to the bump arena on the freestanding targets. */
void *
__pascal_new(int size)
{
    return P_ALLOC((unsigned long)size);
}

void
__pascal_dispose(void *p)
{
    P_FREE(p);
}

struct pstr {
    unsigned int refcount;
    unsigned int kind;
    int len;
    int cap;
    char bytes[1];
};

#define PSTR_HDR    16
#define PSTR_KIND   1u
#define PSTR_STATIC 0xffffffffu

/* Allocate a descriptor of logical length len with room for cap characters
   (cap >= len), so a later in-place append can reuse the spare capacity. */
static struct pstr *
pstr_make_cap(int len, int cap)
{
    struct pstr *d;

    if (len < 0)
        len = 0;
    if (cap < len)
        cap = len;
    if (cap <= 0)
        return 0;
    d = P_ALLOC((unsigned long)PSTR_HDR + (unsigned long)cap + 1u);
    if (!d)
        return 0;
    d->refcount = 1;
    d->kind = PSTR_KIND;
    d->len = len;
    d->cap = cap;
    d->bytes[len] = '\0';
    return d;
}

static struct pstr *
pstr_make(int len)
{
    struct pstr *d;

    if (len <= 0)
        return 0;
    d = P_ALLOC((unsigned long)PSTR_HDR + (unsigned long)len + 1u);
    if (!d)
        return 0;
    d->refcount = 1;
    d->kind = PSTR_KIND;
    d->len = len;
    d->cap = len;
    d->bytes[len] = '\0';
    return d;
}

static int
pstr_len(const void *s)
{
    return s ? ((const struct pstr *)s)->len : 0;
}

static const char *
pstr_bytes(const void *s)
{
    return s ? ((const struct pstr *)s)->bytes : "";
}

void *
__pascal_str_new(const char *b, int len)
{
    struct pstr *d;
    int i;

    if (len < 0)
        len = 0;
    d = pstr_make(len);
    if (d)
        for (i = 0; i < len; i++)
            d->bytes[i] = b[i];
    return d;
}

int
__pascal_str_length(const void *s)
{
    return pstr_len(s);
}

/* A fresh one-character heap string from a character code.  Used when a char
   value is an operand of string concatenation or a self-append. */
void *
__pascal_str_ofchar(int c)
{
    char ch = (char)c;
    return __pascal_str_new(&ch, 1);
}

/* An owned copy of s (a fresh heap string), or NULL for the empty string.
   Assignment uses this to give each variable an independent value. */
void *
__pascal_str_dup(const void *s)
{
    return s ? __pascal_str_new(pstr_bytes(s), pstr_len(s)) : (void *)0;
}

/* Add a reference.  A NULL (the empty string) and a static literal (refcount
   0xffffffff) are immortal and left alone.  Returns s so a caller can retain
   and store in one expression. */
void *
__pascal_str_retain(void *s)
{
    struct pstr *d = s;

    if (d && d->refcount != PSTR_STATIC)
        d->refcount++;
    return s;
}

/* Drop a reference and free the descriptor at zero.  A NULL and a static
   literal are left alone. */
void
__pascal_str_release(void *s)
{
    struct pstr *d = s;

    if (!d || d->refcount == PSTR_STATIC)
        return;
    if (--d->refcount == 0)
        P_FREE(d);
}

/* Return a uniquely owned, writable copy of s for an in-place mutation (copy
   on write).  A value already held by exactly one owner is returned as is; a
   shared or static value is copied to a fresh refcount-1 descriptor and the
   caller's shared reference dropped, so the destination slot ends up owning
   the private copy.  NULL (the empty string) has nothing to mutate. */
void *
__pascal_str_unique(void *s)
{
    struct pstr *d = s;
    void *copy;

    /* refcount 1 means the sole owner (a static literal is 0xffffffff). */
    if (!d || d->refcount == 1u)
        return s;
    copy = __pascal_str_dup(s);
    __pascal_str_release(s);
    return copy;
}

/* Append e to s for the self-append idiom (x := x + e), consuming s's
   reference and returning an owned result.  When s is the sole owner with
   spare capacity the bytes go in place and s is returned; otherwise a fresh
   (over-allocated) string is built and s's reference dropped.  Reading e
   before growing s keeps x := x + x correct. */
void *
__pascal_str_append(void *s, const void *e)
{
    struct pstr *d = s;
    int slen = pstr_len(s);
    int elen = pstr_len(e);
    const char *eb;
    struct pstr *r;
    int i, cap;

    if (elen == 0)
        return s;
    eb = pstr_bytes(e);
    if (d && d->refcount == 1u && d->cap >= slen + elen) {
        for (i = 0; i < elen; i++)
            d->bytes[slen + i] = eb[i];
        d->len = slen + elen;
        d->bytes[d->len] = '\0';
        return d;
    }
    cap = slen + elen;
    cap = cap + cap / 2 + 16;    /* headroom so repeated appends amortize */
    r = pstr_make_cap(slen + elen, cap);
    if (r) {
        const char *sb = pstr_bytes(s);
        for (i = 0; i < slen; i++)
            r->bytes[i] = sb[i];
        for (i = 0; i < elen; i++)
            r->bytes[slen + i] = eb[i];
    }
    __pascal_str_release(s);
    return r;
}

/****************************************************************
 * Dynamic arrays
 *
 * A dynamic array shares the string descriptor layout ({refcount, kind, len,
 * cap} header, elements at +PSTR_HDR) with kind PSTR_KIND_ARR.  len and cap
 * count elements; the retain/release helpers above work unchanged on the
 * header.  NULL is the empty array.  When the elements are managed (an array of
 * string, or of a record with string members) the compiler passes per-element
 * finalize and retain callbacks to the helpers below, so freeing or shrinking a
 * descriptor also drops the references its elements hold.
 ****************************************************************/

#define PSTR_KIND_ARR 2u

static struct pstr *
arr_alloc(int nelem, int elsize, int cap)
{
    struct pstr *d;
    unsigned long bytes;

    if (nelem < 0)
        nelem = 0;
    if (cap < nelem)
        cap = nelem;
    bytes = (unsigned long)PSTR_HDR + (unsigned long)cap * (unsigned long)elsize;
    d = P_ALLOC(bytes);
    if (!d)
        return 0;
    d->refcount = 1;
    d->kind = PSTR_KIND_ARR;
    d->len = nelem;
    d->cap = cap;
    return d;
}

int
__pascal_arr_length(const void *a)
{
    return pstr_len(a);
}

/* SetLength(a, n): resize to n elements, consuming a's reference and returning
   the owned result.  Resizes in place when a is uniquely owned with capacity,
   otherwise copies into a fresh (over-allocated) descriptor and drops the old
   reference (copy on write for a shared array).  New elements are zeroed. */
void *
__pascal_arr_setlength(void *a, int newlen, int elsize)
{
    struct pstr *d = a;
    int oldlen = pstr_len(a);
    struct pstr *r;
    int copyn, i;

    if (newlen <= 0) {
        __pascal_str_release(a);
        return 0;
    }
    if (d && d->refcount == 1u && d->cap >= newlen) {
        char *base = (char *)d + PSTR_HDR;
        for (i = oldlen * elsize; i < newlen * elsize; i++)
            base[i] = 0;
        d->len = newlen;
        return d;
    }
    r = arr_alloc(newlen, elsize, newlen + newlen / 2 + 4);
    if (r) {
        const char *src = pstr_bytes(a);
        char *dst = (char *)r + PSTR_HDR;
        copyn = oldlen < newlen ? oldlen : newlen;
        for (i = 0; i < copyn * elsize; i++)
            dst[i] = src[i];
        for (i = copyn * elsize; i < newlen * elsize; i++)
            dst[i] = 0;
    }
    __pascal_str_release(a);
    return r;
}

/* Element reference counting for an array whose elements are a single managed
   pointer (an array of string, or of a scalar dynamic array): the element at
   `elem` is that pointer.  These are the fixed callbacks the compiler passes
   for such element types, in place of a generated per-record finalizer. */
void
__pascal_elem_release_ptr(void *elem)
{
    __pascal_str_release(*(void **)elem);
}

void
__pascal_elem_retain_ptr(void *elem)
{
    __pascal_str_retain(*(void **)elem);
}

/* Release a reference to a dynamic array of managed elements.  At the last
   reference `dtor` finalizes each live element before the descriptor is freed.
   NULL and static descriptors are left alone, matching __pascal_str_release. */
void
__pascal_arr_release(void *a, int elsize, void (*dtor)(void *))
{
    struct pstr *d = a;
    int i;

    if (!d || d->refcount == PSTR_STATIC)
        return;
    if (--d->refcount == 0) {
        char *base = (char *)d + PSTR_HDR;
        for (i = 0; i < d->len; i++)
            dtor(base + i * elsize);
        P_FREE(d);
    }
}

/* SetLength for a dynamic array of managed elements.  Like
   __pascal_arr_setlength, but it finalizes the elements a shrink drops (dtor)
   and, when copying a shared array (copy on write), retains the managed value
   in each copied element so the old and new descriptors each own a reference.
   A uniquely owned reallocation moves the elements, transferring ownership, so
   the old shell is freed by the plain release without finalizing them. */
void *
__pascal_arr_setlength_mgd(void *a, int newlen, int elsize,
                           void (*dtor)(void *), void (*retain)(void *))
{
    struct pstr *d = a;
    int oldlen = pstr_len(a);
    struct pstr *r;
    int copyn, i;
    char *base;

    if (newlen <= 0) {
        __pascal_arr_release(a, elsize, dtor);
        return 0;
    }
    if (d && d->refcount == 1u && d->cap >= newlen) {
        base = (char *)d + PSTR_HDR;
        if (newlen < oldlen)
            for (i = newlen; i < oldlen; i++)
                dtor(base + i * elsize);
        else
            for (i = oldlen * elsize; i < newlen * elsize; i++)
                base[i] = 0;
        d->len = newlen;
        return d;
    }
    r = arr_alloc(newlen, elsize, newlen + newlen / 2 + 4);
    if (r) {
        const char *src = pstr_bytes(a);
        char *dst = (char *)r + PSTR_HDR;
        copyn = oldlen < newlen ? oldlen : newlen;
        for (i = 0; i < copyn * elsize; i++)
            dst[i] = src[i];
        for (i = copyn * elsize; i < newlen * elsize; i++)
            dst[i] = 0;
        if (d && d->refcount != 1u)       /* shared: the copies now co-own */
            for (i = 0; i < copyn; i++)
                retain(dst + i * elsize);
    }
    __pascal_str_release(a);
    return r;
}

/* The current output stream for Write/WriteLn: 1 (Output) by default, set to 2
   (ErrOutput) around a WriteLn(ErrOutput, ...) by the compiler.  Output is
   unbuffered (direct write), so Flush is a no-op. */
static int g_outfd = 1;

/* Write exactly n bytes to the current output fd, advancing over a short write
   and stopping on error.  Every output path funnels through here, so checking
   write()'s result (the host libc marks it warn_unused_result) and completing a
   partial write are both handled in one place. */
static void
out_write(const char *p, int n)
{
    while (n > 0) {
        int w = write(g_outfd, p, n);
        if (w <= 0)
            break;              /* error, or nothing written: stop this chunk */
        p += w;
        n -= w;
    }
}

void
__pascal_set_outfd(int fd)
{
    g_outfd = fd;
}

void
__pascal_flush(void)
{
}

void
__pascal_write_raw(const char *p, int n)
{
    out_write(p, n);
}

/* Command-line arguments (ParamStr/ParamCount).  main captures argc/argv here
   at entry; a freestanding host that passes none leaves these zero. */
static int g_argc;
static char **g_argv;

void
__pascal_set_args(int argc, char **argv)
{
    g_argc = argc;
    g_argv = argv;
}

/* ParamCount: the number of arguments after the program name. */
int
__pascal_param_count(void)
{
    return g_argc > 0 ? g_argc - 1 : 0;
}

/* ParamStr(i): argument i as a heap string (0 is the program name); an index
   out of range is the empty string. */
void *
__pascal_param_str(int i)
{
    const char *s;
    int n = 0;

    if (i < 0 || i >= g_argc || !g_argv)
        return 0;
    s = g_argv[i];
    if (!s)
        return 0;
    while (s[n])
        n++;
    return __pascal_str_new(s, n);
}

void
__pascal_write_int(int v)
{
    char buf[12];
    int i, neg;
    unsigned int u;

    if (v == 0) {
        out_write("0", 1);
        return;
    }

    neg = 0;
    if (v < 0) {
        neg = 1;
        u = (unsigned int)(-(v + 1)) + 1;
    } else {
        u = (unsigned int)v;
    }

    i = sizeof(buf);
    while (u > 0) {
        buf[--i] = '0' + (u % 10);
        u /= 10;
    }
    if (neg)
        buf[--i] = '-';
    out_write(buf + i, (int)(sizeof(buf) - i));
}

void
__pascal_write_int64(long long v)
{
    char buf[24];
    int i, neg;
    unsigned long long u;

    if (v == 0) {
        out_write("0", 1);
        return;
    }

    neg = 0;
    if (v < 0) {
        neg = 1;
        u = (unsigned long long)(-(v + 1)) + 1;
    } else {
        u = (unsigned long long)v;
    }

    i = sizeof(buf);
    while (u > 0) {
        buf[--i] = '0' + (int)(u % 10);
        u /= 10;
    }
    if (neg)
        buf[--i] = '-';
    out_write(buf + i, (int)(sizeof(buf) - i));
}

/* Field-width writers: right-justify in a field of `width` columns, padding on
   the left with spaces.  Width is a minimum, so a wider value prints in full.
   These use only integer formatting and out_write, so they serve both the host
   and the freestanding builds. */
static void
pad_spaces(int n)
{
    while (n-- > 0)
        out_write(" ", 1);
}

void
__pascal_write_int_w(int v, int width)
{
    char buf[16];
    int i = (int)sizeof(buf), len;
    unsigned u = (v < 0) ? (unsigned)(-(long)v) : (unsigned)v;

    if (u == 0)
        buf[--i] = '0';
    while (u) {
        buf[--i] = (char)('0' + (int)(u % 10));
        u /= 10;
    }
    if (v < 0)
        buf[--i] = '-';
    len = (int)sizeof(buf) - i;
    pad_spaces(width - len);
    out_write(buf + i, len);
}

void
__pascal_write_int64_w(long long v, int width)
{
    char buf[24];
    int i = (int)sizeof(buf), len;
    unsigned long long u = (v < 0) ? (unsigned long long)(-v) : (unsigned long long)v;

    if (u == 0)
        buf[--i] = '0';
    while (u) {
        buf[--i] = (char)('0' + (int)(u % 10));
        u /= 10;
    }
    if (v < 0)
        buf[--i] = '-';
    len = (int)sizeof(buf) - i;
    pad_spaces(width - len);
    out_write(buf + i, len);
}

void
__pascal_write_str_w(const void *s, int width)
{
    int len = pstr_len(s);

    pad_spaces(width - len);
    if (len)
        out_write(pstr_bytes(s), len);
}

void
__pascal_write_char_w(int c, int width)
{
    char ch = (char)c;

    pad_spaces(width - 1);
    out_write(&ch, 1);
}

#ifdef PAS_HOST
void
__pascal_write_real(double v)
{
    char buf[40];
    int n = snprintf(buf, sizeof(buf), "%.15g", v);

    if (n > 0)
        out_write(buf, (int)n);
}

/* write(r:w:d): fixed point with d fractional digits in a field w wide. */
void
__pascal_write_real_fixed(double v, int width, int dec)
{
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%*.*f", width, dec, v);

    if (n > 0)
        out_write(buf, (int)n);
}

/* write(r:w) with no :d: scientific (exponential) form in a field w wide. */
void
__pascal_write_real_sci(double v, int width)
{
    char buf[64];
    int prec = width > 8 ? width - 8 : 6;
    int n = snprintf(buf, sizeof(buf), "%*.*E", width, prec, v);

    if (n > 0)
        out_write(buf, (int)n);
}

void *
__pascal_str_from_real(double v)
{
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%.15g", v);

    if (n < 0)
        n = 0;
    if (n > (int)sizeof(buf))
        n = (int)sizeof(buf);
    return __pascal_str_new(buf, n);
}

int
__pascal_round(double v)
{
    return (int)llround(v);
}

double
__pascal_sqrt(double v)
{
    return sqrt(v);
}

double __pascal_sin(double v)    { return sin(v); }
double __pascal_cos(double v)    { return cos(v); }
double __pascal_ln(double v)     { return log(v); }
double __pascal_exp(double v)    { return exp(v); }
double __pascal_arctan(double v) { return atan(v); }
#else
/* Freestanding (no libc): the ColdFire corpus does not exercise reals, so these
   are minimal placeholders that keep the object link-complete. */
void
__pascal_write_real(double v)
{
    (void)v;
    out_write("0", 1);
}

void
__pascal_write_real_fixed(double v, int width, int dec)
{
    (void)v;
    (void)width;
    (void)dec;
    out_write("0", 1);
}

void
__pascal_write_real_sci(double v, int width)
{
    (void)v;
    (void)width;
    out_write("0", 1);
}

void *
__pascal_str_from_real(double v)
{
    (void)v;
    return __pascal_str_new("0", 1);
}

int
__pascal_round(double v)
{
    return (int)v;
}

double
__pascal_sqrt(double v)
{
    return v;
}

/* Transcendental placeholders: like sqrt above, these keep the freestanding
   object link-complete; the real-exercising tests run on the host backends. */
double __pascal_sin(double v)    { return v; }
double __pascal_cos(double v)    { return v; }
double __pascal_ln(double v)     { return v; }
double __pascal_exp(double v)    { return v; }
double __pascal_arctan(double v) { return v; }
#endif

/****************************************************************
 * Pseudo-random numbers
 *
 * A 32-bit xorshift over `unsigned int`, which is 32 bits on every target, so
 * the sequence from a given seed is identical on the ILP32 and LP64 backends
 * and tests that do not randomize are reproducible everywhere.  random(n)
 * returns an integer in [0, n); bare random returns a real in [0, 1) (host
 * only, since the freestanding targets do not format reals).
 ****************************************************************/

static unsigned int rnd_state = 0x2a3b4c5du;    /* fixed default seed */

static unsigned int
rnd_u32(void)
{
    unsigned int x = rnd_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rnd_state = x;
    return x;
}

int
__pascal_random(int n)
{
    if (n <= 0)
        return 0;
    return (int)(rnd_u32() % (unsigned int)n);
}

#ifdef PAS_HOST
#include <time.h>
void __pascal_randomize(void)
{
    unsigned int t = (unsigned int)time((time_t *)0);
    rnd_state = t ? t : 1u;             /* never seed to zero (xorshift fixpoint) */
}
double __pascal_random_real(void)
{
    return (double)(rnd_u32() >> 8) * (1.0 / 16777216.0);   /* [0, 1) */
}
#else
void __pascal_randomize(void) { }       /* no clock; sequence stays reproducible */
double __pascal_random_real(void) { return 0.0; }
#endif

void
__pascal_write_char(int v)
{
    char c = (char)v;
    out_write(&c, 1);
}

void
__pascal_write_str(const void *s)
{
    int len = pstr_len(s);

    if (len > 0)
        out_write(pstr_bytes(s), len);
}

int
__pascal_str_compare(const void *a, const void *b)
{
    int la = pstr_len(a), lb = pstr_len(b);
    const char *pa = pstr_bytes(a), *pb = pstr_bytes(b);
    int n = la < lb ? la : lb, i;

    for (i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)pa[i];
        unsigned char cb = (unsigned char)pb[i];
        if (ca != cb)
            return (int)ca - (int)cb;
    }
    return la - lb;
}

void *
__pascal_str_concat(const void *a, const void *b)
{
    int la = pstr_len(a), lb = pstr_len(b), i;
    const char *pa = pstr_bytes(a), *pb = pstr_bytes(b);
    struct pstr *d = pstr_make(la + lb);

    if (d) {
        for (i = 0; i < la; i++)
            d->bytes[i] = pa[i];
        for (i = 0; i < lb; i++)
            d->bytes[la + i] = pb[i];
    }
    return d;
}

void *
__pascal_str_copy(const void *s, int index, int count)
{
    int len = pstr_len(s), avail, i;
    const char *p = pstr_bytes(s);
    struct pstr *d;

    if (index < 1)
        index = 1;
    if (count < 0 || index > len)
        count = 0;
    else {
        avail = len - (index - 1);
        if (count > avail)
            count = avail;
    }
    d = pstr_make(count);
    if (d)
        for (i = 0; i < count; i++)
            d->bytes[i] = p[index - 1 + i];
    return d;
}

int
__pascal_str_pos(const void *needle, const void *haystack)
{
    int ln = pstr_len(needle), lh = pstr_len(haystack), i, j;
    const char *pn = pstr_bytes(needle), *ph = pstr_bytes(haystack);

    if (ln == 0 || ln > lh)
        return 0;
    for (i = 0; i + ln <= lh; i++) {
        for (j = 0; j < ln; j++)
            if (ph[i + j] != pn[j])
                break;
        if (j == ln)
            return i + 1;
    }
    return 0;
}

void *
__pascal_str_delete(const void *s, int index, int count)
{
    int len = pstr_len(s), start = index - 1, i, k;
    const char *p = pstr_bytes(s);
    struct pstr *d;

    if (start < 0 || start >= len || count <= 0)
        return __pascal_str_dup(s);
    if (start + count > len)
        count = len - start;
    d = pstr_make(len - count);
    if (d) {
        k = 0;
        for (i = 0; i < start; i++)
            d->bytes[k++] = p[i];
        for (i = start + count; i < len; i++)
            d->bytes[k++] = p[i];
    }
    return d;
}

void *
__pascal_str_insert(const void *src, const void *dst, int index)
{
    int ld = pstr_len(dst), ls = pstr_len(src), pos = index - 1, i, k;
    const char *pd = pstr_bytes(dst), *ps = pstr_bytes(src);
    struct pstr *d;

    if (pos < 0)
        pos = 0;
    if (pos > ld)
        pos = ld;
    d = pstr_make(ld + ls);
    if (d) {
        k = 0;
        for (i = 0; i < pos; i++)
            d->bytes[k++] = pd[i];
        for (i = 0; i < ls; i++)
            d->bytes[k++] = ps[i];
        for (i = pos; i < ld; i++)
            d->bytes[k++] = pd[i];
    }
    return d;
}

void *
__pascal_str_from_int(int v)
{
    char buf[12];
    int i = (int)sizeof(buf), neg = 0;
    unsigned int u;

    if (v == 0)
        return __pascal_str_new("0", 1);
    if (v < 0) {
        neg = 1;
        u = (unsigned int)(-(v + 1)) + 1;
    } else {
        u = (unsigned int)v;
    }
    while (u > 0) {
        buf[--i] = (char)('0' + (u % 10));
        u /= 10;
    }
    if (neg)
        buf[--i] = '-';
    return __pascal_str_new(buf + i, (int)sizeof(buf) - i);
}

void *
__pascal_str_from_int64(long long v)
{
    char buf[24];
    int i = (int)sizeof(buf), neg = 0;
    unsigned long long u;

    if (v == 0)
        return __pascal_str_new("0", 1);
    if (v < 0) {
        neg = 1;
        u = (unsigned long long)(-(v + 1)) + 1;
    } else {
        u = (unsigned long long)v;
    }
    while (u > 0) {
        buf[--i] = (char)('0' + (int)(u % 10));
        u /= 10;
    }
    if (neg)
        buf[--i] = '-';
    return __pascal_str_new(buf + i, (int)sizeof(buf) - i);
}

/****************************************************************
 * Named files (text, typed, and untyped) over the OS file interface
 *
 * A Pascal file variable holds a struct pfile inline; the front end reserves
 * PFILE_SZ bytes for it (pascal/lower.c) and only ever passes its address, so
 * the layout is private here.  A text file is buffered for reads (so eof and
 * eoln can look ahead without consuming) and written unbuffered, like the
 * standard output.  An untyped file transfers fixed-size blocks.  Typed files
 * (file of T) are defined further below, over the same struct pfile.
 ****************************************************************/

#ifdef PAS_HOST
#include <fcntl.h>              /* open flags */
#include <stdlib.h>            /* exit */
static int file_unlink(const char *p) { return remove(p); }
static int file_rename(const char *o, const char *n) { return rename(o, n); }
#elif defined(__skj_wasm__)
/* WASM: the minimal crt the Pascal lane links provides only read, write and
   exit.  Real file I/O needs WASI preopens (runtime/wasi_file.c, a --dir
   grant) the lane does not set up, so the low-level file calls are
   link-complete stubs that fail cleanly.  open returning -1 makes reset and
   rewrite set IOResult rather than trap, and the file-I/O tests are excluded
   from the WASM lane. */
extern void exit(int code);
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0100
#define O_TRUNC  01000
#define O_APPEND 02000
#define SEEK_SET 0
#define SEEK_END 2
#define AT_FDCWD (-100)
static int  open(const char *p, int f, int m) { (void)p; (void)f; (void)m; return -1; }
static int  close(int fd) { (void)fd; return 0; }
static long lseek(int fd, long o, int w) { (void)fd; (void)o; (void)w; return -1; }
static int  file_unlink(const char *p) { (void)p; return -1; }
static int  file_rename(const char *o, const char *n) { (void)o; (void)n; return -1; }
#else
extern int  open(const char *path, int flags, int mode);
extern int  close(int fd);
extern long lseek(int fd, long off, int whence);
extern int  unlinkat(int dirfd, const char *path, int flags);
extern int  renameat2(int od, const char *op, int nd, const char *np, int fl);
extern void exit(int code);
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0100
#define O_TRUNC  01000
#define O_APPEND 02000
#define SEEK_SET 0
#define SEEK_END 2
#define AT_FDCWD (-100)
static int file_unlink(const char *p) { return unlinkat(AT_FDCWD, p, 0); }
static int
file_rename(const char *o, const char *n)
{
    return renameat2(AT_FDCWD, o, AT_FDCWD, n, 0);
}
#endif

#define PF_NAMEMAX 128
#define PF_BUFSZ   256
#define PF_EOF     1
#define PF_WRITE   2
#define PF_UNTYPED 4
#define PF_TYPED   8

struct pfile {
    int  fd;                    /* OS handle; -1 when closed */
    int  flags;                 /* PF_EOF | PF_WRITE | PF_UNTYPED | PF_TYPED */
    int  recsize;               /* untyped record size, or typed component size */
    int  bpos;                  /* text read cursor into buf */
    int  blen;                  /* text: valid bytes in buf */
    int  pos;                   /* typed: current component index */
    char *fbuf;                 /* typed: the f^ component buffer (heap), else 0 */
    char buf[PF_BUFSZ];         /* text read buffer */
    char name[PF_NAMEMAX];      /* path recorded by assign */
};

/* IOResult state.  io_check (from {$I+}/{$I-}) decides whether a failing I/O op
   traps or just records io_result; IOResult returns and clears io_result. */
static int io_check = 1;
static int io_result;

/* The current input file for Read/ReadLn: NULL means the standard input (the
   rd_buf path below), otherwise the named file the front end redirected to. */
static struct pfile *cur_in;

/* Refill a file's read buffer and return the next byte, or -1 at end. */
static int
file_getc(struct pfile *f)
{
    if (f->flags & PF_EOF)
        return -1;
    if (f->bpos >= f->blen) {
        f->blen = read(f->fd, f->buf, sizeof(f->buf));
        f->bpos = 0;
        if (f->blen <= 0) {
            f->flags |= PF_EOF;
            return -1;
        }
    }
    return (unsigned char)f->buf[f->bpos++];
}

/* True when a file has no more bytes; refills the buffer to look ahead without
   consuming, so eof is a true lookahead. */
static int
file_at_eof(struct pfile *f)
{
    if (f->flags & PF_EOF)
        return 1;
    if (f->bpos < f->blen)
        return 0;
    f->blen = read(f->fd, f->buf, sizeof(f->buf));
    f->bpos = 0;
    if (f->blen <= 0) {
        f->flags |= PF_EOF;
        return 1;
    }
    return 0;
}

static char rd_buf[256];
static int rd_pos;
static int rd_len;
static int rd_eof;

static int
rd_getc(void)
{
    if (cur_in)
        return file_getc(cur_in);
    if (rd_eof)
        return -1;
    if (rd_pos >= rd_len) {
        rd_len = read(0, rd_buf, sizeof(rd_buf));
        rd_pos = 0;
        if (rd_len <= 0) {
            rd_eof = 1;
            return -1;
        }
    }
    return (unsigned char)rd_buf[rd_pos++];
}

/* Push the last byte read back onto the current input so the next rd_getc
   returns it again (used after a numeric parse stops at a non-digit). */
static void
rd_ungetc(void)
{
    if (cur_in) {
        if (cur_in->bpos > 0)
            cur_in->bpos--;
    } else if (rd_pos > 0) {
        rd_pos--;
    }
}

void
__pascal_read_char(int *dst)
{
    int c = rd_getc();

    *dst = (c >= 0) ? c : 0;
}

int
__pascal_eof(void)
{
    return rd_eof;
}

void
__pascal_read_int(int *dst)
{
    int c, neg;
    unsigned int v;

    /* skip whitespace (not newlines for read vs readln distinction) */
    for (;;) {
        c = rd_getc();
        if (c < 0) {
            *dst = 0;
            return;
        }
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            break;
    }

    neg = 0;
    if (c == '-') {
        neg = 1;
        c = rd_getc();
    } else if (c == '+') {
        c = rd_getc();
    }

    v = 0;
    while (c >= '0' && c <= '9') {
        v = v * 10 + (unsigned int)(c - '0');
        c = rd_getc();
    }

    /* push back the non-digit character */
    if (c >= 0)
        rd_ungetc();

    *dst = neg ? -(int)v : (int)v;
}

void *
__pascal_read_str(void)
{
    char buf[1024];
    int c, len = 0;

    for (;;) {
        c = rd_getc();
        if (c < 0 || c == '\n')
            break;
        if (len < (int)sizeof(buf))
            buf[len++] = (char)c;
    }
    return __pascal_str_new(buf, len);
}

void
__pascal_readln(void)
{
    int c;

    for (;;) {
        c = rd_getc();
        if (c < 0 || c == '\n')
            return;
    }
}

/* Read a real from the current input: skip leading whitespace, take a signed
   decimal with optional fraction and exponent, and convert with strtod.  It
   reads through rd_getc/rd_ungetc, so it works on the standard input or a
   redirected file, and stops at (and pushes back) the first non-number byte.
   Host only; the freestanding targets do not exercise reals, so the stub keeps
   the object link-complete (like the other real routines above). */
#ifdef PAS_HOST
double
__pascal_read_real(void)
{
    char buf[64];
    int i = 0, c;

    do {
        c = rd_getc();
    } while (c == ' ' || c == '\t' || c == '\n' || c == '\r');
    if (c == '+' || c == '-') {
        if (i < 63) buf[i++] = (char)c;
        c = rd_getc();
    }
    while (c >= '0' && c <= '9') {
        if (i < 63) buf[i++] = (char)c;
        c = rd_getc();
    }
    if (c == '.') {
        if (i < 63) buf[i++] = '.';
        c = rd_getc();
        while (c >= '0' && c <= '9') {
            if (i < 63) buf[i++] = (char)c;
            c = rd_getc();
        }
    }
    if (c == 'e' || c == 'E') {
        if (i < 63) buf[i++] = (char)c;
        c = rd_getc();
        if (c == '+' || c == '-') {
            if (i < 63) buf[i++] = (char)c;
            c = rd_getc();
        }
        while (c >= '0' && c <= '9') {
            if (i < 63) buf[i++] = (char)c;
            c = rd_getc();
        }
    }
    if (c >= 0)
        rd_ungetc();
    buf[i] = '\0';
    return strtod(buf, 0);
}
#else
double
__pascal_read_real(void)
{
    return 0.0;
}
#endif

/* eoln on the standard input: peek the next byte without consuming it. */
int
__pascal_eoln(void)
{
    int c = rd_getc();
    if (c < 0)
        return 1;
    rd_ungetc();
    return c == '\n';
}

/****************************************************************
 * File I/O ABI (named text and untyped files)
 ****************************************************************/

void __pascal_use_in(struct pfile *f)  { cur_in = f; }
void __pascal_std_in(void)             { cur_in = 0; }
void __pascal_use_out(struct pfile *f) { g_outfd = f->fd; }
void __pascal_std_out(void)            { g_outfd = 1; }

void __pascal_set_iocheck(int on)      { io_check = on; }

int
__pascal_io_result(void)
{
    int r = io_result;
    io_result = 0;
    return r;
}

int __pascal_feof(struct pfile *f)  { return file_at_eof(f); }

int
__pascal_feoln(struct pfile *f)
{
    if (file_at_eof(f))
        return 1;
    return f->buf[f->bpos] == '\n';
}

/* SeekEof: skip blanks, tabs and line ends, then report end of input.  The
   skipped whitespace is consumed; the first non-blank byte is pushed back so
   the next read still sees it.  stdin and file forms share the logic. */
int
__pascal_seekeof(void)
{
    for (;;) {
        int c = rd_getc();
        if (c < 0)
            return 1;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            continue;
        rd_ungetc();
        return 0;
    }
}

int
__pascal_fseekeof(struct pfile *f)
{
    for (;;) {
        int c = file_getc(f);
        if (c < 0)
            return 1;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            continue;
        if (f->bpos > 0)
            f->bpos--;
        return 0;
    }
}

/* SeekEoln: skip blanks and tabs on the current line (not line ends), then
   report whether the next byte ends the line (a newline or end of input). */
int
__pascal_seekeoln(void)
{
    for (;;) {
        int c = rd_getc();
        if (c < 0)
            return 1;
        if (c == ' ' || c == '\t')
            continue;
        rd_ungetc();
        return c == '\n';
    }
}

int
__pascal_fseekeoln(struct pfile *f)
{
    for (;;) {
        int c = file_getc(f);
        if (c < 0)
            return 1;
        if (c == ' ' || c == '\t')
            continue;
        if (f->bpos > 0)
            f->bpos--;
        return c == '\n';
    }
}

/* Write to standard error, completing a short write and stopping on error.
   Consuming write()'s result keeps the host libc's warn_unused_result quiet. */
static void
err_write(const char *p, int n)
{
    while (n > 0) {
        int w = write(2, p, n);
        if (w <= 0)
            break;
        p += w;
        n -= w;
    }
}

static int
cstr_len(const char *p)
{
    int n = 0;
    while (p[n])
        n++;
    return n;
}

/* Record an I/O failure.  Under {$I+} (io_check) it reports to standard error
   and aborts; under {$I-} it just sets IOResult so the program can test it. */
static void
io_error(const char *msg, struct pfile *f)
{
    io_result = 2;
    if (!io_check)
        return;
    err_write("I/O error: ", 11);
    err_write(msg, cstr_len(msg));
    if (f->name[0]) {
        err_write(" '", 2);
        err_write(f->name, cstr_len(f->name));
        err_write("'", 1);
    }
    err_write("\n", 1);
    exit(1);
}

static void
copy_name(char *dst, const void *name)
{
    const char *nd = pstr_bytes(name);
    int nl = pstr_len(name), i;

    if (nl > PF_NAMEMAX - 1)
        nl = PF_NAMEMAX - 1;
    for (i = 0; i < nl; i++)
        dst[i] = nd[i];
    dst[i] = 0;
}

void
__pascal_assign(struct pfile *f, const void *name)
{
    f->fd = -1;
    f->flags = 0;
    f->recsize = 0;
    f->bpos = 0;
    f->blen = 0;
    f->pos = 0;
    f->fbuf = 0;
    copy_name(f->name, name);
}

void
__pascal_reset(struct pfile *f)
{
    int h = open(f->name, O_RDONLY, 0);
    if (h < 0) {
        io_error("cannot open", f);     /* soft mode: leave fd = -1 */
        f->fd = -1;
        return;
    }
    f->fd = h;
    f->flags = 0;
    f->bpos = 0;
    f->blen = 0;
}

void
__pascal_rewrite(struct pfile *f)
{
    int h = open(f->name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (h < 0) {
        io_error("cannot create", f);
        f->fd = -1;
        return;
    }
    f->fd = h;
    f->flags = PF_WRITE;
    f->bpos = 0;
    f->blen = 0;
}

/* Open an existing text file for writing at its end.  Like rewrite but it does
   not create or truncate, so earlier contents are kept. */
void
__pascal_append(struct pfile *f)
{
    int h = open(f->name, O_WRONLY | O_APPEND, 0644);
    if (h < 0) {
        io_error("cannot append", f);
        f->fd = -1;
        return;
    }
    f->fd = h;
    f->flags = PF_WRITE;
    f->bpos = 0;
    f->blen = 0;
}

void
__pascal_close(struct pfile *f)
{
    if (f->fd >= 3)                     /* never close a standard stream */
        close(f->fd);
    if (f->fbuf) {                      /* typed: release the f^ buffer */
        __pascal_dispose(f->fbuf);
        f->fbuf = 0;
    }
    f->fd = -1;
}

void
__pascal_reset_untyped(struct pfile *f, int recsize)
{
    int h = open(f->name, O_RDONLY, 0);
    if (h < 0) {
        io_error("cannot open", f);
        f->fd = -1;
        return;
    }
    f->fd = h;
    f->flags = PF_UNTYPED;
    f->recsize = recsize > 0 ? recsize : 1;
}

void
__pascal_rewrite_untyped(struct pfile *f, int recsize)
{
    int h = open(f->name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (h < 0) {
        io_error("cannot create", f);
        f->fd = -1;
        return;
    }
    f->fd = h;
    f->flags = PF_UNTYPED | PF_WRITE;
    f->recsize = recsize > 0 ? recsize : 1;
}

/* blockread(f, buf, count): read count records of recsize bytes into buf.
   Returns the number of whole records read; a short read sets eof. */
int
__pascal_blockread(struct pfile *f, char *buf, int count)
{
    int want = count * f->recsize, done = 0;

    while (want > 0) {
        int n = read(f->fd, buf + done, want);
        if (n <= 0)
            break;
        done += n;
        want -= n;
    }
    if (done < count * f->recsize)
        f->flags |= PF_EOF;
    return done / f->recsize;
}

int
__pascal_blockwrite(struct pfile *f, const char *buf, int count)
{
    int want = count * f->recsize, done = 0;

    while (want > 0) {
        int n = write(f->fd, buf + done, want);
        if (n <= 0)
            break;
        done += n;
        want -= n;
    }
    return done / f->recsize;
}

void
__pascal_erase(struct pfile *f)
{
    if (file_unlink(f->name) != 0)
        io_error("cannot erase", f);
}

void
__pascal_rename(struct pfile *f, const void *newname)
{
    char nb[PF_NAMEMAX];
    int i;

    copy_name(nb, newname);
    if (file_rename(f->name, nb) != 0) {
        io_error("cannot rename", f);
        return;
    }
    for (i = 0; nb[i]; i++)
        f->name[i] = nb[i];
    f->name[i] = 0;
}

/****************************************************************
 * Typed files (file of T): fixed-size binary components with the ISO file
 * buffer variable f^.  fbuf holds f^ (recsize bytes); every access positions
 * the OS cursor absolutely by component index, so reads, writes and in-place
 * updates can interleave.  Managed components are rejected by the front end.
 ****************************************************************/

static void
fbcopy(char *dst, const char *src, int n)
{
    int i;
    for (i = 0; i < n; i++)
        dst[i] = src[i];
}

/* Position the OS cursor at component `index` (absolute, from the start). */
static void
file_seek_to(struct pfile *f, int index)
{
    (void)lseek(f->fd, (long)index * f->recsize, SEEK_SET);
}

/* Read the component at f->pos into fbuf; a short read marks end of file. */
static void
file_read_comp(struct pfile *f)
{
    int n;
    file_seek_to(f, f->pos);
    n = read(f->fd, f->fbuf, f->recsize);
    if (n != f->recsize)
        f->flags |= PF_EOF;
    else
        f->flags &= ~PF_EOF;
}

/* Address of the file buffer variable f^. */
char *__pascal_fbuf(struct pfile *f) { return f->fbuf; }

/* get(f): advance f^ to the next component. */
void
__pascal_fget(struct pfile *f)
{
    f->pos++;
    file_read_comp(f);
}

/* put(f): write the current f^ at f->pos and advance. */
void
__pascal_fput(struct pfile *f)
{
    int w;
    file_seek_to(f, f->pos);
    w = write(f->fd, f->fbuf, f->recsize);
    (void)w;
    f->pos++;
}

/* read(f, v): v := f^; get(f). */
void
__pascal_fread(struct pfile *f, char *dst)
{
    fbcopy(dst, f->fbuf, f->recsize);
    __pascal_fget(f);
}

/* write(f, v): f^ := v; put(f). */
void
__pascal_fwrite(struct pfile *f, const char *src)
{
    fbcopy(f->fbuf, src, f->recsize);
    __pascal_fput(f);
}

/* seek(f, n): position at component n.  In read mode f^ is reloaded. */
void
__pascal_fseek(struct pfile *f, int n)
{
    f->pos = n;
    if (!(f->flags & PF_WRITE))
        file_read_comp(f);
}

int __pascal_ffilepos(struct pfile *f) { return f->pos; }

/* filesize(f): the number of components.  Restores the cursor afterward. */
int
__pascal_ffilesize(struct pfile *f)
{
    long end = lseek(f->fd, 0, SEEK_END);
    file_seek_to(f, f->pos);
    if (end < 0 || f->recsize <= 0)
        return 0;
    return (int)(end / f->recsize);
}

static int
open_typed(struct pfile *f, int compsz, int flags, const char *what)
{
    int h = open(f->name, flags, 0644);
    if (h < 0) {
        io_error(what, f);
        f->fd = -1;
        return -1;
    }
    f->fd = h;
    f->recsize = compsz > 0 ? compsz : 1;
    f->pos = 0;
    f->flags = PF_TYPED;
    f->fbuf = (char *)__pascal_new(f->recsize);
    return 0;
}

void
__pascal_reset_typed(struct pfile *f, int compsz)
{
    if (open_typed(f, compsz, O_RDWR, "cannot open") != 0)
        return;
    file_read_comp(f);                  /* preload f^ with component 0 */
}

void
__pascal_rewrite_typed(struct pfile *f, int compsz)
{
    if (open_typed(f, compsz, O_WRONLY | O_CREAT | O_TRUNC, "cannot create") != 0)
        return;
    f->flags = PF_TYPED | PF_WRITE;
}

/****************************************************************
 * Set operations (256-bit sets, 32 bytes)
 ****************************************************************/

void
__pascal_set_clear(char *s)
{
    for (int i = 0; i < 32; i++)
        s[i] = 0;
}

void
__pascal_set_add(char *s, int elem)
{
    if (elem >= 0 && elem <= 255)
        s[elem >> 3] |= (1 << (elem & 7));
}

void
__pascal_set_addrange(char *s, int lo, int hi)
{
    for (int i = lo; i <= hi; i++)
        if (i >= 0 && i <= 255)
            s[i >> 3] |= (1 << (i & 7));
}

void
__pascal_set_union(char *dst, char *a, char *b)
{
    for (int i = 0; i < 32; i++)
        dst[i] = a[i] | b[i];
}

void
__pascal_set_intersect(char *dst, char *a, char *b)
{
    for (int i = 0; i < 32; i++)
        dst[i] = a[i] & b[i];
}

void
__pascal_set_diff(char *dst, char *a, char *b)
{
    for (int i = 0; i < 32; i++)
        dst[i] = a[i] & ~b[i];
}

int
__pascal_set_in(int elem, char *s)
{
    if (elem < 0 || elem > 255)
        return 0;
    return (s[elem >> 3] >> (elem & 7)) & 1;
}

int
__pascal_set_eq(char *a, char *b)
{
    for (int i = 0; i < 32; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

int
__pascal_set_ne(char *a, char *b)
{
    return !__pascal_set_eq(a, b);
}

int
__pascal_set_subset(char *a, char *b)
{
    for (int i = 0; i < 32; i++)
        if (a[i] & ~b[i])
            return 0;
    return 1;
}

void
__pascal_set_copy(char *dst, char *src)
{
    for (int i = 0; i < 32; i++)
        dst[i] = src[i];
}

void
__pascal_val(const void *s, int *result, int *code)
{
    int len = pstr_len(s);
    const char *p = pstr_bytes(s);
    int i = 0, neg = 0, val = 0;

    while (i < len && p[i] == ' ')
        i++;
    if (i < len && p[i] == '-') {
        neg = 1;
        i++;
    } else if (i < len && p[i] == '+') {
        i++;
    }
    if (i >= len || p[i] < '0' || p[i] > '9') {
        *result = 0;
        *code = i + 1;
        return;
    }
    while (i < len && p[i] >= '0' && p[i] <= '9') {
        val = val * 10 + (p[i] - '0');
        i++;
    }
    if (i < len) {
        *result = 0;
        *code = i + 1;
        return;
    }
    *result = neg ? -val : val;
    *code = 0;
}

/* Val for an Int64 destination: like __pascal_val but 64-bit. */
void
__pascal_val_int64(const void *s, long long *result, int *code)
{
    int len = pstr_len(s);
    const char *p = pstr_bytes(s);
    int i = 0, neg = 0;
    long long val = 0;

    while (i < len && p[i] == ' ')
        i++;
    if (i < len && p[i] == '-') {
        neg = 1;
        i++;
    } else if (i < len && p[i] == '+') {
        i++;
    }
    if (i >= len || p[i] < '0' || p[i] > '9') {
        *result = 0;
        *code = i + 1;
        return;
    }
    while (i < len && p[i] >= '0' && p[i] <= '9') {
        val = val * 10 + (p[i] - '0');
        i++;
    }
    if (i < len) {
        *result = 0;
        *code = i + 1;
        return;
    }
    *result = neg ? -val : val;
    *code = 0;
}

#ifdef PAS_HOST
/* Val for a real destination: strtod over the whole (trimmed) string. */
void
__pascal_val_real(const void *s, double *result, int *code)
{
    int len = pstr_len(s);
    const char *p = pstr_bytes(s);
    char buf[64];
    char *end;
    int i = 0, n = 0;
    double v;

    while (i < len && p[i] == ' ')
        i++;
    while (i < len && n < (int)sizeof(buf) - 1)
        buf[n++] = p[i++];
    buf[n] = '\0';
    if (n == 0) {
        *result = 0;
        *code = 1;
        return;
    }
    v = strtod(buf, &end);
    if (*end != '\0') {
        *result = 0;
        *code = (int)(end - buf) + 1;
        return;
    }
    *result = v;
    *code = 0;
}
#else
void
__pascal_val_real(const void *s, double *result, int *code)
{
    (void)s;
    *result = 0;
    *code = 1;
}
#endif

#ifndef PAS_HOST
void *
memset(void *s, int c, unsigned int n)
{
    unsigned char *p = s;
    while (n--)
        *p++ = (unsigned char)c;
    return s;
}

void *
memmove(void *dst, const void *src, unsigned int n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else if (d > s) {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}
#endif
