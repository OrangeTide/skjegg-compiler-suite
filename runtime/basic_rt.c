/* basic_rt.c : the BASIC runtime (doc/languages/basic.md).

   The small set of C entry points skj-bc lowers PRINT and END to. Freestanding:
   it writes through the start.S write(2) wrapper, like pascal_rt.c, so a program
   links with start.o and this object and needs no libc. INT is a 64-bit value
   and FLOAT is an IEEE 754 double (see lower.c). */

extern int write(int fd, const char *buf, int n);

/* Write exactly n bytes to stdout, advancing over a short write. */
static void
out_write(const char *p, int n)
{
    while (n > 0) {
        int w = write(1, p, n);
        if (w <= 0)
            break;
        p += w;
        n -= w;
    }
}

/* PRINT of a string literal: raw bytes, already unescaped by the compiler. */
void
__basic_write_raw(const char *p, int n)
{
    out_write(p, n);
}

/* Format an INT in full decimal into out (needs >= 24 bytes); returns the
   length. INT is 64-bit, so the full range including LLONG_MIN is handled
   without overflow on the negate. */
static int
fmt_int(long long v, char *out)
{
    char tmp[24];
    int i = sizeof(tmp), n = 0;
    unsigned long long u;

    if (v == 0) {
        out[0] = '0';
        return 1;
    }
    if (v < 0) {
        out[n++] = '-';
        u = (unsigned long long)(-(v + 1)) + 1;   /* negate without overflow */
    } else {
        u = (unsigned long long)v;
    }
    while (u > 0) {
        tmp[--i] = (char)('0' + (u % 10));
        u /= 10;
    }
    while (i < (int)sizeof(tmp))
        out[n++] = tmp[i++];
    return n;
}

/* PRINT of an INT value, in full decimal. */
void
__basic_write_int(long long v)
{
    char buf[24];
    out_write(buf, fmt_int(v, buf));
}

/* Convert a 64-bit INT to a FLOAT, returning the double's bit pattern as a
   64-bit integer. The caller reinterprets those bits as a double (see to_float
   in lower.c); the integer return register convention matches the backend,
   while a double return would not.

   The conversion itself avoids a direct (double)(long long) cast, which pulls in
   the libgcc helper __floatdidf that a freestanding build has no copy of. The
   value is split into two 32-bit halves and recombined; each (double)(int) cast
   is a single hardware convert, and the low half is treated as unsigned by
   adding 2^32 when its sign bit is set. */
long long
__basic_i64tof_bits(long long v)
{
    union { double d; long long i; } u;
    int hi = (int)(v >> 32);
    int lo = (int)(v & 0xffffffff);
    double d = (double)hi * 4294967296.0 + (double)lo;

    if (lo < 0)
        d += 4294967296.0;
    u.d = d;
    return u.i;
}

/* Format a FLOAT into out (needs >= 32 bytes); returns the length. This phase
   uses a fixed six-place decimal with trailing zeros trimmed (at least one
   fractional digit kept). The spec's shortest-round-trip form needs a Ryu or
   Grisu formatter and is deferred. */
static int
fmt_float(double v, char *out)
{
    int ip, f, n = 0, i;
    double frac;
    char fb[6];

    if (v < 0) {
        out[n++] = '-';
        v = -v;
    }
    /* The integer part uses a 32-bit convert (a single hardware instruction; a
       64-bit double->int cast would need the libgcc helper __fixdfdi), so a
       magnitude of 2^31 or more is beyond this phase's printer. */
    ip = (int)v;                            /* v >= 0, truncates */
    frac = v - (double)ip;
    f = (int)(frac * 1000000.0 + 0.5);
    if (f >= 1000000) {                     /* rounding carried into integer */
        f -= 1000000;
        ip += 1;
    }
    n += fmt_int(ip, out + n);              /* ip >= 0, prints no sign */
    out[n++] = '.';
    for (i = 5; i >= 0; i--) {
        fb[i] = (char)('0' + (f % 10));
        f /= 10;
    }
    i = 6;
    while (i > 1 && fb[i - 1] == '0')
        i--;
    for (int j = 0; j < i; j++)
        out[n++] = fb[j];
    return n;
}

/* PRINT of a FLOAT value. */
void
__basic_write_float(double v)
{
    char buf[32];
    out_write(buf, fmt_float(v, buf));
}

/* Integer power for the '^' operator: base ** exp on INT operands. A zero
   exponent is 1; a negative exponent is 0 (integer ^ has no fractional result,
   which the FLOAT phase will handle for FLOAT operands). */
long long
__basic_ipow(long long base, long long exp)
{
    long long r = 1;

    if (exp < 0)
        return 0;
    while (exp > 0) {
        if (exp & 1)
            r *= base;
        base *= base;
        exp >>= 1;
    }
    return r;
}

/* ---- strings ----

   A BASIC STRING is a handle: a pointer to [int length][bytes]. Strings are
   immutable; an operation that changes content allocates a fresh object in the
   shared runtime arena (the bump allocator in start.S). */

extern void *__moo_arena_alloc(int size);

struct bstr {
    int  len;
    char data[];
};

/* Allocate an uninitialized string object of len bytes. */
static struct bstr *
str_alloc(int len)
{
    struct bstr *s = __moo_arena_alloc((int)sizeof(int) + len);
    s->len = len;
    return s;
}

/* Copy n bytes (a whole-record assignment or record argument copy). */
void
__basic_blockcopy(void *dst, void *src, int n)
{
    char *d = dst;
    const char *s = src;
    int i;

    for (i = 0; i < n; i++)
        d[i] = s[i];
}

/* PRINT of a STRING: its bytes. */
void
__basic_write_str(void *p)
{
    struct bstr *s = p;
    out_write(s->data, s->len);
}

/* Concatenate two strings into a new one. */
void *
__basic_str_cat(void *pa, void *pb)
{
    struct bstr *a = pa, *b = pb;
    struct bstr *r = str_alloc(a->len + b->len);
    int i;

    for (i = 0; i < a->len; i++)
        r->data[i] = a->data[i];
    for (i = 0; i < b->len; i++)
        r->data[a->len + i] = b->data[i];
    return r;
}

/* Byte-wise comparison: <0, 0, or >0, like memcmp but length-aware. */
int
__basic_str_cmp(void *pa, void *pb)
{
    struct bstr *a = pa, *b = pb;
    int n = a->len < b->len ? a->len : b->len;
    int i;

    for (i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a->data[i];
        unsigned char cb = (unsigned char)b->data[i];
        if (ca != cb)
            return (int)ca - (int)cb;
    }
    return a->len - b->len;
}

/* LEN(s). */
int
__basic_str_len(void *p)
{
    return ((struct bstr *)p)->len;
}

/* ASC(s): the first byte, or 0 for the empty string. */
int
__basic_asc(void *p)
{
    struct bstr *s = p;
    return s->len > 0 ? (int)(unsigned char)s->data[0] : 0;
}

/* INSTR(hay, needle): 1-based index of the first match, or 0 if not found. An
   empty needle matches at position 1. */
int
__basic_instr(void *ph, void *pn)
{
    struct bstr *h = ph, *n = pn;
    int i, j;

    if (n->len == 0)
        return 1;
    for (i = 0; i + n->len <= h->len; i++) {
        for (j = 0; j < n->len; j++)
            if (h->data[i + j] != n->data[j])
                break;
        if (j == n->len)
            return i + 1;
    }
    return 0;
}

/* CHR$(code): a one-character string of the low 8 bits. */
void *
__basic_chr(int code)
{
    struct bstr *s = str_alloc(1);
    s->data[0] = (char)(code & 0xff);
    return s;
}

/* STR$(int). */
void *
__basic_str_from_int(long long v)
{
    char buf[24];
    int n = fmt_int(v, buf);
    struct bstr *s = str_alloc(n);
    int i;

    for (i = 0; i < n; i++)
        s->data[i] = buf[i];
    return s;
}

/* STR$(float). */
void *
__basic_str_from_float(double v)
{
    char buf[32];
    int n = fmt_float(v, buf);
    struct bstr *s = str_alloc(n);
    int i;

    for (i = 0; i < n; i++)
        s->data[i] = buf[i];
    return s;
}

/* VAL(s): parse a leading decimal number from a string, as a FLOAT (bits). */
long long
__basic_val_bits(void *p)
{
    struct bstr *s = p;
    union { double d; long long i; } u;
    int i = 0, neg = 0;
    double v = 0.0, scale;

    while (i < s->len && (s->data[i] == ' ' || s->data[i] == '\t'))
        i++;
    if (i < s->len && (s->data[i] == '-' || s->data[i] == '+'))
        neg = (s->data[i++] == '-');
    while (i < s->len && s->data[i] >= '0' && s->data[i] <= '9')
        v = v * 10.0 + (s->data[i++] - '0');
    if (i < s->len && s->data[i] == '.') {
        i++;
        scale = 0.1;
        while (i < s->len && s->data[i] >= '0' && s->data[i] <= '9') {
            v += (s->data[i++] - '0') * scale;
            scale *= 0.1;
        }
    }
    u.d = neg ? -v : v;
    return u.i;
}

/* VALINT(s): parse a leading signed integer from a string. */
long long
__basic_valint(void *p)
{
    struct bstr *s = p;
    int i = 0, neg = 0;
    long long v = 0;

    while (i < s->len && (s->data[i] == ' ' || s->data[i] == '\t'))
        i++;
    if (i < s->len && (s->data[i] == '-' || s->data[i] == '+'))
        neg = (s->data[i++] == '-');
    while (i < s->len && s->data[i] >= '0' && s->data[i] <= '9')
        v = v * 10 + (s->data[i++] - '0');
    return neg ? -v : v;
}

/* SPACE$(n): a string of n spaces. */
void *
__basic_space(int n)
{
    struct bstr *r;
    int i;

    if (n < 0)
        n = 0;
    r = str_alloc(n);
    for (i = 0; i < n; i++)
        r->data[i] = ' ';
    return r;
}

/* STRING$(n, s): s repeated n times. */
void *
__basic_string(int n, void *p)
{
    struct bstr *s = p, *r;
    int i, j, k = 0;

    if (n < 0)
        n = 0;
    r = str_alloc(n * s->len);
    for (i = 0; i < n; i++)
        for (j = 0; j < s->len; j++)
            r->data[k++] = s->data[j];
    return r;
}

/* Render v in the given base (2..16) into tmp, most significant digit first,
   as an unsigned 64-bit value; returns the digit count. */
static int
fmt_base(unsigned long long v, int base, char *tmp)
{
    char rev[64];
    int t = 0, i;

    if (v == 0)
        rev[t++] = '0';
    while (v) {
        int d = (int)(v % (unsigned)base);
        rev[t++] = d < 10 ? (char)('0' + d) : (char)('A' + d - 10);
        v /= (unsigned)base;
    }
    for (i = 0; i < t; i++)
        tmp[i] = rev[t - 1 - i];
    return t;
}

/* HEX$(n): uppercase hexadecimal of n as a 64-bit unsigned bit pattern. */
void *
__basic_hex(long long v)
{
    char tmp[64];
    int n = fmt_base((unsigned long long)v, 16, tmp);
    struct bstr *r = str_alloc(n);
    int i;

    for (i = 0; i < n; i++)
        r->data[i] = tmp[i];
    return r;
}

/* BIN$(n): binary of n as a 64-bit unsigned bit pattern. */
void *
__basic_bin(long long v)
{
    char tmp[64];
    int n = fmt_base((unsigned long long)v, 2, tmp);
    struct bstr *r = str_alloc(n);
    int i;

    for (i = 0; i < n; i++)
        r->data[i] = tmp[i];
    return r;
}

/* FIX$(x, d): x formatted with exactly d decimal places (d >= 0), rounded. The
   integer part uses the 32-bit range, as the FLOAT printer does. */
void *
__basic_fix(double x, int d)
{
    char buf[48];
    int n = 0, i, ip;
    double p = 1.0, frac;
    struct bstr *r;

    if (d < 0)
        d = 0;
    if (x < 0.0) {
        buf[n++] = '-';
        x = -x;
    }
    for (i = 0; i < d; i++)
        p *= 10.0;
    x += 0.5 / p;                       /* round at the last kept place */
    ip = (int)x;
    n += fmt_base((unsigned long long)(unsigned)ip, 10, buf + n);
    if (d > 0) {
        buf[n++] = '.';
        frac = x - (double)ip;
        for (i = 0; i < d; i++) {
            int dig;
            frac *= 10.0;
            dig = (int)frac;
            if (dig > 9)
                dig = 9;
            buf[n++] = (char)('0' + dig);
            frac -= dig;
        }
    }
    r = str_alloc(n);
    for (i = 0; i < n; i++)
        r->data[i] = buf[i];
    return r;
}

/* ZERO$(n, w): n formatted as a decimal zero-padded to width w (the sign, when
   present, counts toward the width and precedes the zeros). */
void *
__basic_zero(long long v, int w)
{
    char mag[24];
    int neg = v < 0, ml, pad, n = 0, i;
    unsigned long long uv = neg ? (unsigned long long)(-(v + 1)) + 1
                                : (unsigned long long)v;
    struct bstr *r;

    ml = fmt_base(uv, 10, mag);
    pad = w - ml - (neg ? 1 : 0);
    if (pad < 0)
        pad = 0;
    r = str_alloc((neg ? 1 : 0) + pad + ml);
    if (neg)
        r->data[n++] = '-';
    for (i = 0; i < pad; i++)
        r->data[n++] = '0';
    for (i = 0; i < ml; i++)
        r->data[n++] = mag[i];
    return r;
}

/* RJUST$(s, w): s right-justified in width w (space-padded on the left). A
   string already at least w long is returned unchanged. */
void *
__basic_rjust(void *p, int w)
{
    struct bstr *s = p, *r;
    int pad, i, n = 0;

    pad = w - s->len;
    if (pad <= 0)
        return s;
    r = str_alloc(w);
    for (i = 0; i < pad; i++)
        r->data[n++] = ' ';
    for (i = 0; i < s->len; i++)
        r->data[n++] = s->data[i];
    return r;
}

/* LJUST$(s, w): s left-justified in width w (space-padded on the right). */
void *
__basic_ljust(void *p, int w)
{
    struct bstr *s = p, *r;
    int pad, i, n = 0;

    pad = w - s->len;
    if (pad <= 0)
        return s;
    r = str_alloc(w);
    for (i = 0; i < s->len; i++)
        r->data[n++] = s->data[i];
    for (i = 0; i < pad; i++)
        r->data[n++] = ' ';
    return r;
}

/* Copy a byte range [from, from+len) of src into a new string. */
static void *
str_slice(struct bstr *src, int from, int len)
{
    struct bstr *r;
    int i;

    if (from < 0)
        from = 0;
    if (from > src->len)
        from = src->len;
    if (len < 0 || len > src->len - from)
        len = src->len - from;
    r = str_alloc(len);
    for (i = 0; i < len; i++)
        r->data[i] = src->data[from + i];
    return r;
}

/* LEFT$(s, n): the first n characters. */
void *
__basic_left(void *p, int n)
{
    return str_slice(p, 0, n);
}

/* RIGHT$(s, n): the last n characters. */
void *
__basic_right(void *p, int n)
{
    struct bstr *s = p;
    if (n < 0)
        n = 0;
    if (n > s->len)
        n = s->len;
    return str_slice(s, s->len - n, n);
}

/* MID$(s, start, len): from the 1-based start, len characters (len < 0 means to
   the end of the string). */
void *
__basic_mid(void *p, int start, int len)
{
    if (start < 1)
        start = 1;
    return str_slice(p, start - 1, len);
}

/* UCASE$(s) / LCASE$(s): ASCII case folding. */
void *
__basic_ucase(void *p)
{
    struct bstr *s = p;
    struct bstr *r = str_alloc(s->len);
    int i;

    for (i = 0; i < s->len; i++) {
        char c = s->data[i];
        r->data[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }
    return r;
}

void *
__basic_lcase(void *p)
{
    struct bstr *s = p;
    struct bstr *r = str_alloc(s->len);
    int i;

    for (i = 0; i < s->len; i++) {
        char c = s->data[i];
        r->data[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    return r;
}

/* ---- INPUT ----

   One line is read into a static buffer; each designator then consumes the next
   comma-separated field (leading and trailing blanks trimmed). */

extern int read(int fd, char *buf, int n);

#define INBUF_MAX 256
static char inbuf[INBUF_MAX];
static int  inlen, inpos;

/* Read one line from stdin (up to the newline or EOF) into the buffer. */
void
__basic_input_begin(void)
{
    char c;

    inlen = 0;
    inpos = 0;
    while (read(0, &c, 1) == 1) {
        if (c == '\n')
            break;
        if (inlen < INBUF_MAX - 1)
            inbuf[inlen++] = c;
    }
}

/* Advance over the next field; *start gets its first index, the return value is
   the index past its last non-blank character. A trailing comma is consumed. */
static int
next_field(int *start)
{
    int end;

    while (inpos < inlen &&
           (inbuf[inpos] == ' ' || inbuf[inpos] == '\t' || inbuf[inpos] == ','))
        inpos++;
    *start = inpos;
    while (inpos < inlen && inbuf[inpos] != ',')
        inpos++;
    end = inpos;
    while (end > *start && (inbuf[end - 1] == ' ' || inbuf[end - 1] == '\t'))
        end--;
    if (inpos < inlen && inbuf[inpos] == ',')
        inpos++;
    return end;
}

/* INPUT of an INT: the next field parsed as a signed decimal. */
long long
__basic_input_int(void)
{
    int s, e = next_field(&s), i = s, neg = 0;
    long long v = 0;

    if (i < e && (inbuf[i] == '-' || inbuf[i] == '+'))
        neg = (inbuf[i++] == '-');
    for (; i < e && inbuf[i] >= '0' && inbuf[i] <= '9'; i++)
        v = v * 10 + (inbuf[i] - '0');
    return neg ? -v : v;
}

/* INPUT of a FLOAT: the next field parsed as a decimal, returned as bits. */
long long
__basic_input_float_bits(void)
{
    union { double d; long long i; } u;
    int s, e = next_field(&s), i = s, neg = 0;
    double v = 0.0, scale;

    if (i < e && (inbuf[i] == '-' || inbuf[i] == '+'))
        neg = (inbuf[i++] == '-');
    for (; i < e && inbuf[i] >= '0' && inbuf[i] <= '9'; i++)
        v = v * 10.0 + (inbuf[i] - '0');
    if (i < e && inbuf[i] == '.') {
        i++;
        scale = 0.1;
        for (; i < e && inbuf[i] >= '0' && inbuf[i] <= '9'; i++) {
            v += (inbuf[i] - '0') * scale;
            scale *= 0.1;
        }
    }
    u.d = neg ? -v : v;
    return u.i;
}

/* INPUT of a STRING: the next field as a new string. */
void *
__basic_input_str(void)
{
    int s, e = next_field(&s);
    struct bstr *r = str_alloc(e - s);
    int i;

    for (i = 0; i < e - s; i++)
        r->data[i] = inbuf[s + i];
    return r;
}

/* The ',' print separator advances to the next item with a single space. The
   column-aligned tabbing of the spec is a later refinement. */
void
__basic_write_sp(void)
{
    out_write(" ", 1);
}

/* End a PRINT line. */
void
__basic_write_nl(void)
{
    out_write("\n", 1);
}

/* ---- core numeric built-ins ----
 *
 * The integer helpers return a 64-bit result directly (the C ABI returns it in
 * d0:d1, which the backend's i64 return convention matches). The float helpers
 * return the double result's bit pattern as a long long; the caller
 * reinterprets those bits as a double (see bits_to_float in lower.c), because
 * the C ABI returns a double in d0:d1, not the backend's FP return register. */

long long
__basic_iabs(long long x)
{
    return x < 0 ? -x : x;
}

long long
__basic_fabs_bits(double x)
{
    union { double d; long long i; } u;
    u.d = x < 0.0 ? -x : x;
    return u.i;
}

long long
__basic_isgn(long long x)
{
    return x < 0 ? -1 : (x > 0 ? 1 : 0);
}

long long
__basic_fsgn(double x)
{
    return x < 0.0 ? -1 : (x > 0.0 ? 1 : 0);
}

long long
__basic_imin(long long a, long long b)
{
    return a < b ? a : b;
}

long long
__basic_imax(long long a, long long b)
{
    return a > b ? a : b;
}

long long
__basic_fmin_bits(double a, double b)
{
    union { double d; long long i; } u;
    u.d = a < b ? a : b;
    return u.i;
}

long long
__basic_fmax_bits(double a, double b)
{
    union { double d; long long i; } u;
    u.d = a > b ? a : b;
    return u.i;
}

long long
__basic_iclamp(long long x, long long lo, long long hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

long long
__basic_fclamp_bits(double x, double lo, double hi)
{
    union { double d; long long i; } u;
    u.d = x < lo ? lo : (x > hi ? hi : x);
    return u.i;
}

/* INT(x): truncate a FLOAT toward zero to an INT. The conversion goes through a
   32-bit cast (inline on the FPU), so the result is correct for values in the
   signed 32-bit range; a wider magnitude is out of range, matching the
   documented FLOAT print limit. FLOOR, CEIL, and ROUND share the same range. */
long long
__basic_ftoi(double x)
{
    return (long long)(int)x;
}

long long
__basic_floor(double x)
{
    int i = (int)x;                     /* truncates toward zero */
    return (x < 0.0 && (double)i != x) ? i - 1 : i;
}

long long
__basic_ceil(double x)
{
    int i = (int)x;
    return (x > 0.0 && (double)i != x) ? i + 1 : i;
}

long long
__basic_round(double x)
{
    return (long long)(int)(x < 0.0 ? x - 0.5 : x + 0.5);
}

/* LERP(a, b, t) = a + (b - a) * t, as a FLOAT (bit pattern). */
long long
__basic_lerp_bits(double a, double b, double t)
{
    union { double d; long long i; } u;
    u.d = a + (b - a) * t;
    return u.i;
}

/* SQR(x): square root by Newton's iteration, so the freestanding runtime needs
   no libm. A non-positive argument yields 0. */
long long
__basic_sqr_bits(double x)
{
    union { double d; long long i; } u;
    double g;
    int i;

    if (x <= 0.0) {
        u.d = 0.0;
        return u.i;
    }
    g = x < 1.0 ? 1.0 : x;              /* a guess at or above the root */
    for (i = 0; i < 100; i++) {
        double ng = 0.5 * (g + x / g);
        if (ng == g)
            break;
        g = ng;
    }
    u.d = g;
    return u.i;
}

/* ---- trigonometry ----
 *
 * The freestanding runtime carries its own sine, cosine, and arctangent so it
 * needs no libm. Values reduce to a small interval where a short polynomial is
 * accurate past the six decimal places the FLOAT print shows. */

#define BR_PI      3.14159265358979323846
#define BR_HALFPI  1.57079632679489661923
#define BR_QTRPI   0.78539816339744830961
#define BR_TANPI8  0.41421356237309504880   /* tan(pi/8) */
#define BR_DEG2RAD 0.01745329251994329577

/* sin on |x| <= pi/4 (Taylor series). */
static double
sin_core(double x)
{
    double x2 = x * x;
    return x * (1.0 + x2 * (-1.0 / 6.0 + x2 * (1.0 / 120.0
             + x2 * (-1.0 / 5040.0 + x2 * (1.0 / 362880.0)))));
}

/* cos on |x| <= pi/4 (Taylor series). */
static double
cos_core(double x)
{
    double x2 = x * x;
    return 1.0 + x2 * (-0.5 + x2 * (1.0 / 24.0 + x2 * (-1.0 / 720.0
             + x2 * (1.0 / 40320.0))));
}

/* Round x to the nearest integer as a double (|x| within the 32-bit range,
   which covers the reduced quadrant count for any sane angle). */
static double
near_int(double x)
{
    return (double)(int)(x < 0.0 ? x - 0.5 : x + 0.5);
}

/* sin(x) by reducing x to a quadrant offset r in [-pi/4, pi/4]. */
static double
sin_reduced(double x)
{
    double n = near_int(x / BR_HALFPI);
    double r = x - n * BR_HALFPI;
    int q = ((int)n) & 3;
    if (q < 0)
        q += 4;
    switch (q) {
    case 0:  return sin_core(r);
    case 1:  return cos_core(r);
    case 2:  return -sin_core(r);
    default: return -cos_core(r);
    }
}

static double
cos_reduced(double x)
{
    return sin_reduced(x + BR_HALFPI);
}

/* arctangent on |x| <= 1 (minimax polynomial, ~7 digit accuracy). */
static double
atan_core(double x)
{
    double x2 = x * x;
    return x * (0.9998660 + x2 * (-0.3302995 + x2 * (0.1801410
             + x2 * (-0.0851330 + x2 * 0.0208351))));
}

/* arctangent for x >= 0, reduced so atan_core sees |t| <= tan(pi/8), where the
   polynomial is accurate past six decimal places. Values above 1 fold through
   1/x; values in (tan(pi/8), 1] fold through atan(x) = pi/4 + atan((x-1)/(x+1)). */
static double
atan_nonneg(double x)
{
    if (x > 1.0)
        return BR_HALFPI - atan_nonneg(1.0 / x);
    if (x > BR_TANPI8)
        return BR_QTRPI + atan_core((x - 1.0) / (x + 1.0));
    return atan_core(x);
}

static double
atan_full(double x)
{
    return x < 0.0 ? -atan_nonneg(-x) : atan_nonneg(x);
}

static double
atan2_full(double y, double x)
{
    if (x > 0.0)
        return atan_full(y / x);
    if (x < 0.0)
        return y >= 0.0 ? atan_full(y / x) + BR_PI
                        : atan_full(y / x) - BR_PI;
    /* x == 0 */
    if (y > 0.0)
        return BR_HALFPI;
    if (y < 0.0)
        return -BR_HALFPI;
    return 0.0;
}

long long
__basic_sin_bits(double x)
{
    union { double d; long long i; } u;
    u.d = sin_reduced(x);
    return u.i;
}

long long
__basic_cos_bits(double x)
{
    union { double d; long long i; } u;
    u.d = cos_reduced(x);
    return u.i;
}

long long
__basic_tan_bits(double x)
{
    union { double d; long long i; } u;
    double c = cos_reduced(x);
    u.d = c == 0.0 ? 0.0 : sin_reduced(x) / c;
    return u.i;
}

long long
__basic_atn_bits(double x)
{
    union { double d; long long i; } u;
    u.d = atan_full(x);
    return u.i;
}

long long
__basic_atn2_bits(double y, double x)
{
    union { double d; long long i; } u;
    u.d = atan2_full(y, x);
    return u.i;
}

long long
__basic_sind_bits(double deg)
{
    union { double d; long long i; } u;
    u.d = sin_reduced(deg * BR_DEG2RAD);
    return u.i;
}

long long
__basic_cosd_bits(double deg)
{
    union { double d; long long i; } u;
    u.d = cos_reduced(deg * BR_DEG2RAD);
    return u.i;
}

long long
__basic_tand_bits(double deg)
{
    union { double d; long long i; } u;
    double r = deg * BR_DEG2RAD;
    double c = cos_reduced(r);
    u.d = c == 0.0 ? 0.0 : sin_reduced(r) / c;
    return u.i;
}

/* ---- RND, SEED, TIME ----
 *
 * RND draws from a 64-bit linear congruential generator, auto-seeded from the
 * kernel on first use and reseedable with SEED. TIME reads the monotonic clock.
 * The runtime leans on the shared syscall seam (clock_gettime, getrandom); no
 * libc is required. */

extern int getrandom(void *buf, int len, unsigned flags);

struct brt_timespec {
    int sec;
    int nsec;
};

/* A 64-bit timespec, the only clock layout qemu-m68k's user emulation serves:
   the legacy 32-bit clock_gettime (syscall 263) returns EFAULT there, while
   clock_gettime64 (403) fills {tv_sec, tv_nsec} as 64-bit fields. The shared
   syscall seam keeps the 32-bit wrapper for its other consumers, so TIME issues
   the 64-bit call itself. */
struct brt_timespec64 {
    long long sec;
    long long nsec;
};

static int
basic_clock_gettime64(int clk_id, struct brt_timespec64 *ts)
{
    register long d0 __asm__("d0") = 403;       /* __NR_clock_gettime64 */
    register long d1 __asm__("d1") = clk_id;
    register long d2 __asm__("d2") = (long)ts;

    __asm__ volatile ("trap #0"
                      : "+d"(d0)
                      : "d"(d1), "d"(d2)
                      : "memory");
    return (int)d0;
}

static unsigned long long rng_state;
static int rng_inited;

static void
rng_ensure(void)
{
    if (!rng_inited) {
        unsigned long long s = 0;
        getrandom(&s, (int)sizeof s, 0);
        rng_state = s ? s : 0x0123456789abcdefULL;
        rng_inited = 1;
    }
}

static unsigned long long
rng_next(void)
{
    rng_state = rng_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return rng_state;
}

/* SEED(n): set the generator state and return n. */
long long
__basic_seed(long long n)
{
    rng_state = (unsigned long long)n;
    rng_inited = 1;
    return n;
}

/* RND: a FLOAT in [0, 1), with 32-bit resolution. The top word of the state is
   turned into a double through signed halves (no unsigned->double helper). */
long long
__basic_rnd_bits(void)
{
    union { double d; long long i; } u;
    int hi;
    double d;

    rng_ensure();
    hi = (int)(unsigned)(rng_next() >> 32);
    d = (double)hi;
    if (hi < 0)
        d += 4294967296.0;
    u.d = d / 4294967296.0;
    return u.i;
}

/* TIME: seconds on the monotonic clock, as a FLOAT. Programs take differences
   to measure elapsed time. */
long long
__basic_time_bits(void)
{
    union { double d; long long i; } u;
    struct brt_timespec64 ts;
    int sec, nsec;
    double d;

    ts.sec = 0;
    ts.nsec = 0;
    basic_clock_gettime64(1, &ts);      /* CLOCK_MONOTONIC */
    /* The seconds fit the signed 32-bit range (monotonic uptime), so convert
       through a 32-bit cast rather than a (double)(long long) that would pull
       in libgcc. */
    sec = (int)ts.sec;
    nsec = (int)ts.nsec;
    d = (double)sec;
    if (sec < 0)
        d += 4294967296.0;
    u.d = d + (double)nsec * 1e-9;
    return u.i;
}

/* WAIT ms: pause for the given number of milliseconds via nanosleep. A
   non-positive value returns at once. */
extern int nanosleep(const void *req, void *rem);

void
__basic_wait(int ms)
{
    struct brt_timespec req;

    if (ms <= 0)
        return;
    req.sec = ms / 1000;
    req.nsec = (ms % 1000) * 1000000;
    nanosleep(&req, 0);
}
