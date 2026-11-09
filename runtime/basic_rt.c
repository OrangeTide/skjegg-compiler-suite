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

/* Round a double result to true double precision. On a strict-IEEE double
   target (RISC-V, MIPS soft float, x86-64, arm64) this is the identity; on the
   m68k 68881, whose registers hold 80-bit extended values, the volatile store
   drops the excess precision so an intermediate computation rounds the same way
   on every target. The iterative and polynomial math below wraps each step in
   it so the shortest-round-trip FLOAT output is identical across backends. */
static double
force_round(double x)
{
    volatile double v = x;
    return v;
}

/* ---- shortest round-trip FLOAT formatting (Dragon4) ----

   A FLOAT prints as the shortest decimal digit string that reads back to the
   same double (doc/languages/basic.md). The generator is the Steele and White
   Dragon4 algorithm: it works in exact integers (a small base-2^32 bignum), so
   the output depends only on the IEEE 754 bit pattern and is identical on every
   target, hardware float and soft float alike. No libm, no double-to-integer
   conversion helper, and no 64-bit divide: the digit loop divides by repeated
   subtraction. */

#define BN_LIMBS 40                         /* 1280 bits: covers the double range */

struct bignum {
    int n;                                  /* number of significant limbs */
    unsigned d[BN_LIMBS];                   /* little-endian base-2^32 limbs */
};

static void
bn_norm(struct bignum *a)
{
    while (a->n > 0 && a->d[a->n - 1] == 0)
        a->n--;
}

static void
bn_copy(struct bignum *dst, const struct bignum *src)
{
    int i;

    dst->n = src->n;
    for (i = 0; i < src->n; i++)
        dst->d[i] = src->d[i];
}

static void
bn_set_u64(struct bignum *a, unsigned long long v)
{
    a->n = 0;
    while (v) {
        a->d[a->n++] = (unsigned)v;
        v >>= 32;
    }
}

static void
bn_shl(struct bignum *a, int bits)
{
    int words = bits >> 5, b = bits & 31, i;
    unsigned carry;

    if (a->n == 0)
        return;
    if (words) {
        for (i = a->n - 1; i >= 0; i--)
            a->d[i + words] = a->d[i];
        for (i = 0; i < words; i++)
            a->d[i] = 0;
        a->n += words;
    }
    if (b) {
        carry = 0;
        for (i = words; i < a->n; i++) {
            unsigned long long cur = ((unsigned long long)a->d[i] << b) | carry;
            a->d[i] = (unsigned)cur;
            carry = (unsigned)(cur >> 32);
        }
        if (carry)
            a->d[a->n++] = carry;
    }
    bn_norm(a);
}

static void
bn_mul_small(struct bignum *a, unsigned m)
{
    unsigned long long carry = 0;
    int i;

    for (i = 0; i < a->n; i++) {
        unsigned long long cur = (unsigned long long)a->d[i] * m + carry;
        a->d[i] = (unsigned)cur;
        carry = cur >> 32;
    }
    while (carry) {
        a->d[a->n++] = (unsigned)carry;
        carry >>= 32;
    }
    bn_norm(a);
}

static void
bn_add(struct bignum *a, const struct bignum *b)
{
    int i, n = a->n > b->n ? a->n : b->n;
    unsigned long long carry = 0;

    for (i = 0; i < n; i++) {
        unsigned long long cur = carry;
        if (i < a->n)
            cur += a->d[i];
        if (i < b->n)
            cur += b->d[i];
        a->d[i] = (unsigned)cur;
        carry = cur >> 32;
    }
    a->n = n;
    if (carry)
        a->d[a->n++] = (unsigned)carry;
    bn_norm(a);
}

/* out = a + b. */
static void
bn_addto(struct bignum *out, const struct bignum *a, const struct bignum *b)
{
    bn_copy(out, a);
    bn_add(out, b);
}

static int
bn_cmp(const struct bignum *a, const struct bignum *b)
{
    int i;

    if (a->n != b->n)
        return a->n < b->n ? -1 : 1;
    for (i = a->n - 1; i >= 0; i--)
        if (a->d[i] != b->d[i])
            return a->d[i] < b->d[i] ? -1 : 1;
    return 0;
}

/* a -= b, requires a >= b. */
static void
bn_sub(struct bignum *a, const struct bignum *b)
{
    int i;
    long long borrow = 0;

    for (i = 0; i < a->n; i++) {
        long long cur = (long long)a->d[i] - borrow - (i < b->n ? (long long)b->d[i] : 0);
        if (cur < 0) {
            cur += (long long)1 << 32;
            borrow = 1;
        } else {
            borrow = 0;
        }
        a->d[i] = (unsigned)cur;
    }
    bn_norm(a);
}

static void
bn_mul_pow10(struct bignum *a, int k)
{
    while (k-- > 0)
        bn_mul_small(a, 10u);
}

/* Dragon4: write the shortest significant digits of a finite v > 0 to dig[]
   (most significant first, first digit nonzero), set *pexp10 to the decimal
   exponent of the first digit (value is dig[0].dig[1]... x 10^exp10), and
   return the digit count. */
static int
dragon4(double v, char *dig, int *pexp10)
{
    union { double d; unsigned long long u; } uu;
    unsigned long long bits, frac, f;
    int biased, e, k, n = 0, even, lower_closer, i, L;
    struct bignum R, S, Mp, Mm, t1, t2;

    uu.d = v;
    bits = uu.u;
    frac = bits & (((unsigned long long)1 << 52) - 1);
    biased = (int)((bits >> 52) & 0x7ff);
    if (biased == 0) {                      /* subnormal */
        f = frac;
        e = 1 - 1023 - 52;
    } else {
        f = frac | ((unsigned long long)1 << 52);
        e = biased - 1023 - 52;
    }
    even = ((f & 1) == 0);
    lower_closer = (frac == 0 && biased > 1);   /* predecessor is 2x closer */

    /* Scaled so the half-ulp gaps Mp (up) and Mm (down) are integers. */
    if (e >= 0) {
        if (!lower_closer) {
            bn_set_u64(&R, f); bn_shl(&R, e + 1);
            bn_set_u64(&S, 2);
            bn_set_u64(&Mp, 1); bn_shl(&Mp, e);
            bn_copy(&Mm, &Mp);
        } else {
            bn_set_u64(&R, f); bn_shl(&R, e + 2);
            bn_set_u64(&S, 4);
            bn_set_u64(&Mp, 1); bn_shl(&Mp, e + 1);
            bn_set_u64(&Mm, 1); bn_shl(&Mm, e);
        }
    } else {
        if (!lower_closer) {
            bn_set_u64(&R, f); bn_shl(&R, 1);
            bn_set_u64(&S, 1); bn_shl(&S, 1 - e);
            bn_set_u64(&Mp, 1);
            bn_set_u64(&Mm, 1);
        } else {
            bn_set_u64(&R, f); bn_shl(&R, 2);
            bn_set_u64(&S, 1); bn_shl(&S, 2 - e);
            bn_set_u64(&Mp, 2);
            bn_set_u64(&Mm, 1);
        }
    }

    /* Estimate k ~ ceil(log10(v)) with fixed-point log10(2) ~ 1233/4096; the
       exact value is pinned by the integer fixups below, so the estimate only
       needs to be close. */
    L = 0;
    { unsigned long long ff = f; while (ff) { L++; ff >>= 1; } }
    {
        long num = (long)(e + L - 1) * 1233;
        k = (int)(num >> 12);
        if (num < 0 && (num & 4095))
            k -= 1;                         /* arithmetic floor for negatives */
        k += 1;
    }

    if (k >= 0) {
        bn_mul_pow10(&S, k);
    } else {
        int m = -k;
        bn_mul_pow10(&R, m);
        bn_mul_pow10(&Mp, m);
        bn_mul_pow10(&Mm, m);
    }

    /* Pin k so that 1/10 <= R/S < 1 (boundary aware). */
    for (;;) {
        bn_addto(&t1, &R, &Mp);
        if (even ? bn_cmp(&t1, &S) >= 0 : bn_cmp(&t1, &S) > 0) {
            bn_mul_small(&S, 10u);
            k++;
        } else {
            break;
        }
    }
    for (;;) {
        bn_addto(&t1, &R, &Mp);
        bn_copy(&t2, &t1);
        bn_mul_small(&t2, 10u);
        if (even ? bn_cmp(&t2, &S) <= 0 : bn_cmp(&t2, &S) < 0) {
            bn_mul_small(&R, 10u);
            bn_mul_small(&Mp, 10u);
            bn_mul_small(&Mm, 10u);
            k--;
        } else {
            break;
        }
    }

    *pexp10 = k - 1;
    for (;;) {
        int d, low, high;

        bn_mul_small(&R, 10u);
        bn_mul_small(&Mp, 10u);
        bn_mul_small(&Mm, 10u);
        d = 0;                              /* d = floor(R/S), R %= S */
        while (bn_cmp(&R, &S) >= 0) {
            bn_sub(&R, &S);
            d++;
        }
        low = even ? bn_cmp(&R, &Mm) <= 0 : bn_cmp(&R, &Mm) < 0;
        bn_addto(&t1, &R, &Mp);
        high = even ? bn_cmp(&t1, &S) >= 0 : bn_cmp(&t1, &S) > 0;
        if (low || high) {
            if (low && !high) {
                /* keep d */
            } else if (high && !low) {
                d++;
            } else {
                bn_copy(&t2, &R);
                bn_mul_small(&t2, 2u);
                if (bn_cmp(&t2, &S) >= 0)
                    d++;
            }
            dig[n++] = (char)('0' + d);
            break;
        }
        dig[n++] = (char)('0' + d);
    }

    /* A terminal round-up may carry past '9'; propagate it and shift on overflow. */
    i = n - 1;
    while (i >= 0 && dig[i] == '0' + 10) {
        dig[i] = '0';
        if (i == 0) {
            dig[0] = '1';
            (*pexp10)++;
        } else {
            dig[i - 1]++;
        }
        i--;
    }
    while (n > 1 && dig[n - 1] == '0')       /* a carry can leave one trailing 0 */
        n--;
    return n;
}

/* Format a FLOAT into out (needs >= 32 bytes); returns the length. Fixed-point
   notation with at least one fractional digit, switching to scientific for a
   decimal exponent outside [-4, 17), the way a "%g" of shortest width does. */
static int
fmt_float(double v, char *out)
{
    char dig[20];
    int n = 0, sig, exp10, i, pos;

    if (v != v) {                           /* NaN */
        out[0] = 'n'; out[1] = 'a'; out[2] = 'n';
        return 3;
    }
    if (v < 0) {
        out[n++] = '-';
        v = -v;
    }
    if (v == 0.0) {
        out[n++] = '0'; out[n++] = '.'; out[n++] = '0';
        return n;
    }
    if (v > 1.7976931348623157e308) {       /* +Inf */
        out[n++] = 'i'; out[n++] = 'n'; out[n++] = 'f';
        return n;
    }

    sig = dragon4(v, dig, &exp10);

    if (exp10 < -4 || exp10 >= 17) {        /* scientific */
        int ex;
        out[n++] = dig[0];
        out[n++] = '.';
        if (sig > 1)
            for (i = 1; i < sig; i++)
                out[n++] = dig[i];
        else
            out[n++] = '0';
        out[n++] = 'e';
        out[n++] = exp10 < 0 ? '-' : '+';
        ex = exp10 < 0 ? -exp10 : exp10;
        if (ex >= 100) {
            out[n++] = (char)('0' + ex / 100);
            ex %= 100;
        }
        out[n++] = (char)('0' + ex / 10);
        out[n++] = (char)('0' + ex % 10);
    } else if (exp10 >= 0) {                /* fixed, integer part present */
        for (pos = 0; pos <= exp10; pos++)
            out[n++] = (pos < sig) ? dig[pos] : '0';
        out[n++] = '.';
        if (exp10 + 1 < sig)
            for (i = exp10 + 1; i < sig; i++)
                out[n++] = dig[i];
        else
            out[n++] = '0';
    } else {                                /* fixed, 0.0ddd */
        out[n++] = '0';
        out[n++] = '.';
        for (i = 0; i < (-exp10 - 1); i++)
            out[n++] = '0';
        for (i = 0; i < sig; i++)
            out[n++] = dig[i];
    }
    return n;
}

/* ---- correctly rounded decimal parsing (the inverse of Dragon4) ----

   VAL and INPUT read a FLOAT back from text. To honour the round-trip promise
   (the shortest printed form reads back to the same double) the parse has to be
   correctly rounded, which the old digit-by-digit accumulation was not. The
   significand is gathered as an exact integer and the value integer x 10^exp is
   rounded to the nearest double by exact big-integer division, round to nearest,
   ties to even. Like the printer this is integer-only and identical on every
   target. */

/* Round (neg ? -1 : 1) * M * 10^dec_exp to the nearest double, returning its
   bit pattern. M is the integer significand (at most 19 digits, so it fits a
   64-bit word); dec_exp is the power of ten applied to it. */
static long long
make_double(int neg, unsigned long long M, int dec_exp)
{
    union { double d; unsigned long long u; } uu;
    struct bignum num, den, t;
    int bin_exp, i, biased, round_bit, sticky;
    unsigned long long q;

    if (M == 0) {
        uu.u = neg ? ((unsigned long long)1 << 63) : 0;
        return (long long)uu.u;
    }

    bn_set_u64(&num, M);
    bn_set_u64(&den, 1);
    if (dec_exp >= 0)
        bn_mul_pow10(&num, dec_exp);
    else
        bn_mul_pow10(&den, -dec_exp);

    /* Normalise so that den <= num < 2*den; bin_exp is the leading bit's power. */
    bin_exp = 0;
    while (bn_cmp(&num, &den) < 0) {
        bn_shl(&num, 1);
        bin_exp--;
    }
    for (;;) {
        bn_copy(&t, &den);
        bn_shl(&t, 1);
        if (bn_cmp(&num, &t) >= 0) {
            bn_shl(&den, 1);
            bin_exp++;
        } else {
            break;
        }
    }

    /* Extract 53 significand bits by restoring binary long division. */
    q = 0;
    for (i = 0; i < 53; i++) {
        q <<= 1;
        if (bn_cmp(&num, &den) >= 0) {
            bn_sub(&num, &den);
            q |= 1;
        }
        bn_shl(&num, 1);
    }
    round_bit = (bn_cmp(&num, &den) >= 0);       /* the 54th bit */
    if (round_bit)
        bn_sub(&num, &den);
    sticky = (num.n > 0);                         /* anything below it */

    biased = bin_exp + 1023;                       /* q is 1.m x 2^bin_exp */
    if (biased >= 2047) {                           /* overflow to infinity */
        uu.u = (unsigned long long)2047 << 52;
    } else if (biased >= 1) {                       /* normal, round to even */
        if (round_bit && (sticky || (q & 1))) {
            q++;
            if (q == ((unsigned long long)1 << 53)) {
                q >>= 1;
                biased++;
            }
        }
        if (biased >= 2047)
            uu.u = (unsigned long long)2047 << 52;
        else
            uu.u = ((unsigned long long)biased << 52)
                 | (q & (((unsigned long long)1 << 52) - 1));
    } else {                                        /* subnormal, shift right */
        int sh = 1 - biased;                        /* >= 1 */
        if (sh >= 54) {
            uu.u = 0;                               /* underflow to zero */
        } else {
            int rb = (int)((q >> (sh - 1)) & 1);
            int st = ((q & (((unsigned long long)1 << (sh - 1)) - 1)) != 0)
                     || round_bit || sticky;
            q >>= sh;
            if (rb && (st || (q & 1)))
                q++;                                /* 2^52 rolls up to the
                                                       smallest normal, which is
                                                       the same bit pattern */
            uu.u = q;
        }
    }
    if (neg)
        uu.u |= (unsigned long long)1 << 63;
    return (long long)uu.u;
}

/* Parse a leading decimal number from d[0..len) (optional sign, integer part,
   fraction, and 'e'/'E' exponent) into a FLOAT bit pattern. Leading blanks are
   skipped; parsing stops at the first character that does not continue a number.
   The significand is capped at 19 digits, enough to round any double correctly
   (a double needs at most 17), with further integer digits folded into the
   exponent and further fraction digits dropped. */
static long long
parse_decimal(const char *d, int len)
{
    unsigned long long M = 0;
    int i = 0, neg = 0, fdig = 0, intdrop = 0, esign = 1, eval = 0, dec_exp;

    while (i < len && (d[i] == ' ' || d[i] == '\t'))
        i++;
    if (i < len && (d[i] == '-' || d[i] == '+'))
        neg = (d[i++] == '-');
    while (i < len && d[i] >= '0' && d[i] <= '9') {
        if (M < 1000000000000000000ULL)
            M = M * 10 + (unsigned)(d[i] - '0');
        else
            intdrop++;                              /* keep the magnitude */
        i++;
    }
    if (i < len && d[i] == '.') {
        i++;
        while (i < len && d[i] >= '0' && d[i] <= '9') {
            if (M < 1000000000000000000ULL) {
                M = M * 10 + (unsigned)(d[i] - '0');
                fdig++;
            }
            i++;
        }
    }
    if (i < len && (d[i] == 'e' || d[i] == 'E')) {
        int j = i + 1;
        if (j < len && (d[j] == '-' || d[j] == '+'))
            esign = (d[j++] == '-') ? -1 : 1;
        if (j < len && d[j] >= '0' && d[j] <= '9') {    /* only if digits follow */
            while (j < len && d[j] >= '0' && d[j] <= '9')
                eval = eval * 10 + (d[j++] - '0');
            i = j;
        }
    }
    dec_exp = esign * eval - fdig + intdrop;
    return make_double(neg, M, dec_exp);
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

    return parse_decimal(s->data, s->len);
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
   integer part uses a 32-bit convert, so a magnitude of 2^31 or more is out of
   range (unlike PRINT, which prints any FLOAT in full). */
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
    int s, e = next_field(&s);

    return parse_decimal(inbuf + s, e - s);
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
   signed 32-bit range; a wider magnitude is out of range. FLOOR, CEIL, and ROUND
   share the same range. */
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
        double ng = force_round(0.5 * force_round(g + force_round(x / g)));
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
 * accurate to about seven to ten significant digits; the shortest-round-trip
 * printer shows that limit in the trailing digits. Every arithmetic step is
 * wrapped in force_round so the result is identical on every backend, including
 * the m68k 68881 whose registers would otherwise carry extra precision. */

#define BR_PI      3.14159265358979323846
#define BR_HALFPI  1.57079632679489661923
#define BR_QTRPI   0.78539816339744830961
#define BR_TANPI8  0.41421356237309504880   /* tan(pi/8) */
#define BR_DEG2RAD 0.01745329251994329577

/* Horner evaluation of a polynomial in x2, highest-degree coefficient first,
   rounding each step to double so the result is identical on every backend. */
static double
poly(const double *c, int n, double x2)
{
    double acc = c[0];
    int i;

    for (i = 1; i < n; i++)
        acc = force_round(force_round(acc * x2) + c[i]);
    return acc;
}

/* sin on |x| <= pi/4 (Taylor series). */
static double
sin_core(double x)
{
    static const double c[5] = {
        1.0 / 362880.0, -1.0 / 5040.0, 1.0 / 120.0, -1.0 / 6.0, 1.0
    };
    double x2 = force_round(x * x);
    return force_round(x * poly(c, 5, x2));
}

/* cos on |x| <= pi/4 (Taylor series). */
static double
cos_core(double x)
{
    static const double c[5] = {
        1.0 / 40320.0, -1.0 / 720.0, 1.0 / 24.0, -0.5, 1.0
    };
    double x2 = force_round(x * x);
    return poly(c, 5, x2);
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
    double n = near_int(force_round(x / BR_HALFPI));
    double r = force_round(x - force_round(n * BR_HALFPI));
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
    return sin_reduced(force_round(x + BR_HALFPI));
}

/* arctangent on |x| <= 1 (minimax polynomial, ~7 digit accuracy). */
static double
atan_core(double x)
{
    static const double c[5] = {
        0.0208351, -0.0851330, 0.1801410, -0.3302995, 0.9998660
    };
    double x2 = force_round(x * x);
    return force_round(x * poly(c, 5, x2));
}

/* arctangent for x >= 0, reduced so atan_core sees |t| <= tan(pi/8), where the
   polynomial is accurate past six decimal places. Values above 1 fold through
   1/x; values in (tan(pi/8), 1] fold through atan(x) = pi/4 + atan((x-1)/(x+1)). */
static double
atan_nonneg(double x)
{
    if (x > 1.0)
        return force_round(BR_HALFPI - atan_nonneg(force_round(1.0 / x)));
    if (x > BR_TANPI8)
        return force_round(BR_QTRPI
            + atan_core(force_round(force_round(x - 1.0) / force_round(x + 1.0))));
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
        return atan_full(force_round(y / x));
    if (x < 0.0)
        return y >= 0.0 ? force_round(atan_full(force_round(y / x)) + BR_PI)
                        : force_round(atan_full(force_round(y / x)) - BR_PI);
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
    u.d = c == 0.0 ? 0.0 : force_round(sin_reduced(x) / c);
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
    u.d = sin_reduced(force_round(deg * BR_DEG2RAD));
    return u.i;
}

long long
__basic_cosd_bits(double deg)
{
    union { double d; long long i; } u;
    u.d = cos_reduced(force_round(deg * BR_DEG2RAD));
    return u.i;
}

long long
__basic_tand_bits(double deg)
{
    union { double d; long long i; } u;
    double r = force_round(deg * BR_DEG2RAD);
    double c = cos_reduced(r);
    u.d = c == 0.0 ? 0.0 : force_round(sin_reduced(r) / c);
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

/* A 64-bit timespec. The 32-bit targets (ColdFire, RV32) take the time64 clock
   call, clock_gettime64 (403), which fills {tv_sec, tv_nsec} as 64-bit fields:
   on ColdFire the legacy 32-bit clock_gettime (263) returns EFAULT under
   qemu-m68k, and RV32 is a modern 32-bit ABI with no legacy 32-bit time syscall
   at all. The shared syscall seam keeps a 32-bit wrapper for its other
   consumers, so TIME issues the 64-bit call itself, per the host's syscall ABI
   (m68k trap #0 with d0/d1/d2; RISC-V ecall with a7/a0/a1). */
struct brt_timespec64 {
    long long sec;
    long long nsec;
};

/* MIPS is a legacy 32-bit arch: it keeps the ordinary clock_gettime (a 32-bit
   timespec) and nanosleep, so it needs no time64 workaround and reaches both
   through the shared seam (clock_gettime from the crt), the way the other seam
   calls (write, read, getrandom, nanosleep) are already resolved. The MIPS
   runtime is built by skj-cc-mips-sf (which predefines __skj_mips__), not gcc
   (which predefines __mips__), so accept either spelling. */
#if defined(__mips__) || defined(__skj_mips__)
#define BASIC_MIPS 1
#endif

extern int clock_gettime(int clk_id, void *ts);

#if !defined(BASIC_MIPS)
static int
basic_clock_gettime64(int clk_id, struct brt_timespec64 *ts)
{
#if defined(__riscv)
    register long a7 __asm__("a7") = 403;       /* __NR_clock_gettime64 */
    register long a0 __asm__("a0") = clk_id;
    register long a1 __asm__("a1") = (long)ts;

    __asm__ volatile ("ecall"
                      : "+r"(a0)
                      : "r"(a7), "r"(a1)
                      : "memory");
    return (int)a0;
#elif defined(__x86_64__) || defined(__aarch64__)
    /* LP64: the native clock_gettime fills a 16-byte {long sec; long nsec}
       timespec, which is exactly brt_timespec64 here, so call it through the
       shared seam directly. */
    return clock_gettime(clk_id, ts);
#else
    register long d0 __asm__("d0") = 403;       /* __NR_clock_gettime64 */
    register long d1 __asm__("d1") = clk_id;
    register long d2 __asm__("d2") = (long)ts;

    __asm__ volatile ("trap #0"
                      : "+d"(d0)
                      : "d"(d1), "d"(d2)
                      : "memory");
    return (int)d0;
#endif
}
#endif

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
    int sec, nsec;
    double d;

#if defined(BASIC_MIPS)
    struct brt_timespec ts;             /* MIPS: ordinary 32-bit-timespec clock */

    ts.sec = 0;
    ts.nsec = 0;
    clock_gettime(1, &ts);              /* CLOCK_MONOTONIC */
    sec = ts.sec;
    nsec = ts.nsec;
#else
    struct brt_timespec64 ts;

    ts.sec = 0;
    ts.nsec = 0;
    basic_clock_gettime64(1, &ts);      /* CLOCK_MONOTONIC */
    /* The seconds fit the signed 32-bit range (monotonic uptime), so convert
       through a 32-bit cast rather than a (double)(long long) that would pull
       in libgcc. */
    sec = (int)ts.sec;
    nsec = (int)ts.nsec;
#endif
    d = (double)sec;
    if (sec < 0)
        d += 4294967296.0;
    u.d = d + (double)nsec * 1e-9;
    return u.i;
}

/* WAIT ms: pause for the given number of milliseconds. ColdFire takes the
   legacy nanosleep with a 32-bit timespec (it works under qemu-m68k). RV32 is a
   modern 32-bit ABI with no legacy nanosleep (syscall 101 is absent), so it
   uses the time64 clock_nanosleep (407) with a 64-bit timespec, a relative
   sleep on the monotonic clock, mirroring the clock_gettime64 TIME path. A
   non-positive value returns at once. */
extern int nanosleep(const void *req, void *rem);

void
__basic_wait(int ms)
{
    if (ms <= 0)
        return;
#if defined(__riscv)
    {
        struct brt_timespec64 req;
        register long a7 __asm__("a7") = 407;   /* __NR_clock_nanosleep_time64 */
        register long a0 __asm__("a0") = 1;     /* CLOCK_MONOTONIC */
        register long a1 __asm__("a1") = 0;     /* relative sleep */
        register long a2 __asm__("a2");
        register long a3 __asm__("a3") = 0;     /* no remainder */

        req.sec = ms / 1000;
        req.nsec = (long long)(ms % 1000) * 1000000;
        a2 = (long)&req;
        __asm__ volatile ("ecall"
                          : "+r"(a0)
                          : "r"(a7), "r"(a1), "r"(a2), "r"(a3)
                          : "memory");
    }
#elif defined(__x86_64__) || defined(__aarch64__)
    {
        /* LP64: nanosleep takes a 16-byte {long sec; long nsec} timespec, which
           is brt_timespec64 here. */
        struct brt_timespec64 req;

        req.sec = ms / 1000;
        req.nsec = (long long)(ms % 1000) * 1000000;
        nanosleep(&req, 0);
    }
#else
    {
        struct brt_timespec req;

        req.sec = ms / 1000;
        req.nsec = (ms % 1000) * 1000000;
        nanosleep(&req, 0);
    }
#endif
}
