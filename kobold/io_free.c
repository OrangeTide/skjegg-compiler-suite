/* io_free.c : the freestanding console and number surface for a Kobold guest.

   The re-bottoming of kp_host.c for a host whose only output seam is the write
   syscall and whose only input seam is read.  kp_host.c backs these on stdio
   (fprintf, fgetc); here they go straight to the seam, with no libc.  The number
   formatting and parsing (Str, Val) cannot be shared with the hosted str_fmt.c,
   which uses snprintf and strtod, so the integer side is hand-rolled here (the
   same approach as the upstream sandbox); the real side arrives with the
   soft-float layer.

   The syscall wrappers are the one target-specific piece.  Each is a single trap
   (trap #0 on ColdFire, ecall on rv32) with register-bound operands, which needs
   extended inline asm, so they live in per-target assembly beside this file
   (runtime/io_free_cf.S, runtime/io_free_rv.S) and are declared here.  Streams:
   fd 1 is standard output, fd 2 standard error. */

#include "str.h"
#include "rt_glue.h"

#include <stdint.h>
#include <stddef.h>

/****************************************************************
 * The host seam: write, read, exit
 ****************************************************************/

/* The wrappers are defined in the per-target syscall assembly
   (runtime/io_free_cf.S, runtime/io_free_rv.S).  ColdFire additionally has
   open/close, so it enables the file-I/O surface at the end of this file; the
   rv32 console seam has write/read/exit only. */
#if (defined(__riscv) && __riscv_xlen == 32) || defined(__skj_riscv__)
/* rv32: console seam only. */
#elif defined(__mcoldfire__) || defined(__skj_m68k__)
#define KP_HAVE_FILE_IO 1
#else
#error "io_free.c is a freestanding console; build it with the rv32 or ColdFire toolchain"
#endif

long kp_sys_write(long fd, const void *buf, long len);
long kp_sys_read(long fd, void *buf, long len);
void kp_sys_exit(int code);
#ifdef KP_HAVE_FILE_IO
long kp_sys_open(const char *path, long flags, long mode);
long kp_sys_close(long fd);
#endif

/****************************************************************
 * Console output (the io_* names the guest emits)
 ****************************************************************/

void
io_write_str(int fd, uint32_t s)
{
    long f = (fd == 2) ? 2 : 1;
    int32_t len = kp_str_len(s);

    if (len > 0)
        kp_sys_write(f, (const void *)(uintptr_t)kp_str_base(s), len);
}

void
io_write_char(int fd, int ch)
{
    char c = (char)ch;
    long f = (fd == 2) ? 2 : 1;

    kp_sys_write(f, &c, 1);
}

void
io_write_ln(int fd)
{
    io_write_char(fd, '\n');
}

/* A decimal integer straight to the seam, with no heap string.  INT_MIN is safe:
   the magnitude is taken in unsigned. */
void
io_write_int(int fd, int v)
{
    char buf[12];
    int n = 0;
    uint32_t m = v < 0 ? 0u - (uint32_t)v : (uint32_t)v;
    long f = (fd == 2) ? 2 : 1;
    char out[12];
    int i, j = 0;

    do {
        buf[n++] = (char)('0' + (int)(m % 10u));
        m /= 10u;
    } while (m);
    if (v < 0)
        out[j++] = '-';
    for (i = 0; i < n; i++)
        out[j++] = buf[n - 1 - i];
    kp_sys_write(f, out, j);
}

/* fd streams are unbuffered here, so a flush is a no-op; halt exits. */
void io_flush(int fd) { (void)fd; }
void io_halt(int code) { kp_sys_exit(code); }

/****************************************************************
 * Console input (over the read syscall, one byte at a time)
 ****************************************************************/

static int
read_byte(void)
{
    unsigned char c;

    if (kp_sys_read(0, &c, 1) == 1)
        return c;
    return -1;                  /* end of input */
}

static int g_peek = -2;         /* -2 empty, -1 EOF, else a pushed-back byte */

static int
next_byte(void)
{
    int c;

    if (g_peek != -2) {
        c = g_peek;
        g_peek = -2;
        return c;
    }
    return read_byte();
}

static void
push_byte(int c)
{
    g_peek = c;
}

static int
skip_space(void)
{
    int c;

    do {
        c = next_byte();
    } while (c == ' ' || c == '\t' || c == '\n' || c == '\r');
    return c;
}

int64_t
io_read_int(void)
{
    int c = skip_space();
    int neg = 0;
    int64_t v = 0;

    if (c == '+' || c == '-') {
        neg = c == '-';
        c = next_byte();
    }
    while (c >= '0' && c <= '9') {
        v = v * 10 + (c - '0');
        c = next_byte();
    }
    if (c != -1)
        push_byte(c);
    return neg ? -v : v;
}

int
io_read_char(void)
{
    return next_byte();
}

void
io_read_ln(void)
{
    int c;

    do {
        c = next_byte();
    } while (c != '\n' && c != -1);
}

uint32_t
io_read_str(void)
{
    char buf[256];
    int n = 0;
    int c = next_byte();

    while (c != '\n' && c != -1 && n < (int)sizeof buf)
        buf[n++] = (char)c, c = next_byte();
    return kp_str_new(buf, n);
}

/****************************************************************
 * Str and Val for integers (hand-rolled, no libc; the real side is in the
 * soft-float layer).  Ported from the upstream sandbox platform.
 ****************************************************************/

static int
digit_of(char c, int base)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (base == 16 && c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (base == 16 && c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void
scan_int(const char *b, int32_t len, int64_t *iv, int32_t *code)
{
    int32_t i = 0, start, neg = 0, base = 10, any = 0, ovf = 0;
    uint32_t v = 0, limit;
    int d;

    *iv = 0;
    if (len > 0 && (b[0] == '+' || b[0] == '-')) { neg = b[0] == '-'; i = 1; }
    if (i < len && b[i] == '$') { base = 16; i++; }
    for (start = i; i < len; i++) {
        d = digit_of(b[i], base);
        if (d < 0) break;
        if (v > (0xFFFFFFFFu - (uint32_t)d) / (uint32_t)base) ovf = 1;
        else v = v * (uint32_t)base + (uint32_t)d;
        any = 1;
    }
    if (!any) { *code = start + 1; return; }
    if (i != len) { *code = i + 1; return; }
    limit = neg ? 0x80000000u : 0x7FFFFFFFu;
    if (ovf || v > limit) { *code = len + 1; return; }
    *iv = neg ? -(int64_t)v : (int64_t)v;
    *code = 0;
}

static void
scan_int64(const char *b, int32_t len, int64_t *iv, int32_t *code)
{
    int32_t i = 0, start, neg = 0, base = 10, any = 0, ovf = 0;
    uint64_t v = 0, limit;
    int d;

    *iv = 0;
    if (len > 0 && (b[0] == '+' || b[0] == '-')) { neg = b[0] == '-'; i = 1; }
    if (i < len && b[i] == '$') { base = 16; i++; }
    for (start = i; i < len; i++) {
        d = digit_of(b[i], base);
        if (d < 0) break;
        any = 1;
        if (base == 16) {
            if (v > (0xFFFFFFFFFFFFFFFFull >> 4)) { ovf = 1; continue; }
            v = (v << 4) + (uint64_t)d;
        } else {
            if (v > 0x1999999999999999ull ||
                (v == 0x1999999999999999ull && d > 5)) { ovf = 1; continue; }
            v = (v << 3) + (v << 1) + (uint64_t)d;
        }
    }
    if (!any) { *code = start + 1; return; }
    if (i != len) { *code = i + 1; return; }
    limit = neg ? 0x8000000000000000ull : 0x7FFFFFFFFFFFFFFFull;
    if (ovf || v > limit) { *code = len + 1; return; }
    *iv = neg ? -(int64_t)v : (int64_t)v;
    *code = 0;
}

/* Divide *n by 10 returning the remainder, in constant 64-bit shifts and 32-bit
   divides (16 bits at a time, MSB first), so no 64-bit libgcc helper is pulled. */
static uint32_t
u64_divmod10(uint64_t *n)
{
    uint64_t v = *n, q = 0;
    uint32_t rem = 0, cur;

    cur = (rem << 16) | (uint32_t)((v >> 48) & 0xFFFF); q = (q << 16) | (cur / 10u); rem = cur % 10u;
    cur = (rem << 16) | (uint32_t)((v >> 32) & 0xFFFF); q = (q << 16) | (cur / 10u); rem = cur % 10u;
    cur = (rem << 16) | (uint32_t)((v >> 16) & 0xFFFF); q = (q << 16) | (cur / 10u); rem = cur % 10u;
    cur = (rem << 16) | (uint32_t)(v & 0xFFFF);         q = (q << 16) | (cur / 10u); rem = cur % 10u;
    *n = q;
    return rem;
}

uint32_t
kp_str_from_int(int32_t x)
{
    char tmp[16], buf[16];
    int n = 0, i;
    uint32_t v = x < 0 ? 0u - (uint32_t)x : (uint32_t)x;

    do {
        tmp[n++] = (char)('0' + (int)(v % 10u));
        v /= 10u;
    } while (v);
    if (x < 0)
        tmp[n++] = '-';
    for (i = 0; i < n; i++)
        buf[i] = tmp[n - 1 - i];
    return kp_str_new(buf, n);
}

uint32_t
kp_str_from_int64(int64_t x)
{
    char tmp[24], buf[24];
    int n = 0, i;
    uint64_t v = x < 0 ? 0ull - (uint64_t)x : (uint64_t)x;

    do {
        tmp[n++] = (char)('0' + (int)u64_divmod10(&v));
    } while (v);
    if (x < 0)
        tmp[n++] = '-';
    for (i = 0; i < n; i++)
        buf[i] = tmp[n - 1 - i];
    return kp_str_new(buf, n);
}

int32_t
kp_str_val_int(uint32_t s)
{
    int64_t iv;
    int32_t code;

    scan_int(kp_str_bytes(s), kp_str_len(s), &iv, &code);
    (void)code;
    return (int32_t)iv;
}

int64_t
kp_str_val_int64(uint32_t s)
{
    int64_t iv;
    int32_t code;

    scan_int64(kp_str_bytes(s), kp_str_len(s), &iv, &code);
    (void)code;
    return iv;
}

static double scan_real(const char *b, int32_t len);   /* defined below */

/* Pascal Val's error code: 0 for a fully valid parse, else the 1-based offset of
   the offending character (kind 0 int32, 1 real, 2 int64).  This tier's scan_real
   is lenient and reports no code, so a real parse is taken as valid, matching the
   tier's existing real handling. */
int32_t
kp_str_valcode(uint32_t s, int32_t kind)
{
    int64_t iv;
    int32_t code;

    if (kind == 1) {
        (void)scan_real(kp_str_bytes(s), kp_str_len(s));
        return 0;
    }
    if (kind == 2)
        scan_int64(kp_str_bytes(s), kp_str_len(s), &iv, &code);
    else
        scan_int(kp_str_bytes(s), kp_str_len(s), &iv, &code);
    return code;
}

/****************************************************************
 * Reals: formatting, parsing, and the rv soft-float bit bridge
 *
 * The hosted tier formats a real with the C library's "%g" and parses it with
 * strtod (kp_host.c / str_fmt.c).  This tier has neither, so it hand-rolls a %g
 * matching formatter (six significant digits, trailing zeros stripped) and a
 * double parse.  The rv32 soft-float ABI carries a double across a call as its
 * 64-bit integer bits, not in a float register, so the guest reaches these
 * through the brv_* bridge, which converts the bits to a double and back.
 ****************************************************************/

#if (defined(__riscv) && __riscv_xlen == 32) || defined(__skj_riscv__)
static double
b2d(uint64_t b)
{
    double d;
    memcpy(&d, &b, sizeof d);
    return d;
}

static uint64_t
d2b(double d)
{
    uint64_t b;
    memcpy(&b, &d, sizeof b);
    return b;
}
#endif

/* Format v like C's "%g" with the default precision of six significant digits:
   round to six, then render in fixed notation when the exponent is in [-4, 6)
   and in exponential notation otherwise, stripping trailing fraction zeros and a
   bare decimal point.  Returns the byte count written to out (no NUL). */
static int
fmt_g(double v, char *out)
{
    int n = 0, i, exp10 = 0, last;
    long d;
    double scaled;
    char dig[6];

    if (v != v) { out[0] = 'n'; out[1] = 'a'; out[2] = 'n'; return 3; }
    if (v < 0.0) { out[n++] = '-'; v = -v; }
    if (v == 0.0) { out[n++] = '0'; return n; }

    /* Normalise the magnitude to [1, 10) and count the decimal exponent. */
    while (v >= 10.0) { v /= 10.0; exp10++; }
    while (v < 1.0)  { v *= 10.0; exp10--; }

    /* Round to six significant digits: a 6-digit integer in [100000, 999999],
       handling a carry that pushes it to seven digits (9.999995 -> 10). */
    scaled = v * 100000.0;
    d = (long)(scaled + 0.5);
    if (d >= 1000000) { d = 100000; exp10++; }
    for (i = 5; i >= 0; i--) { dig[i] = (char)('0' + (int)(d % 10)); d /= 10; }

    if (exp10 < -4 || exp10 >= 6) {             /* exponential */
        out[n++] = dig[0];
        last = 5; while (last > 0 && dig[last] == '0') last--;
        if (last >= 1) { out[n++] = '.'; for (i = 1; i <= last; i++) out[n++] = dig[i]; }
        out[n++] = 'e';
        out[n++] = exp10 < 0 ? '-' : '+';
        {
            int e = exp10 < 0 ? -exp10 : exp10;
            if (e >= 100) { out[n++] = (char)('0' + e / 100); e %= 100; }
            out[n++] = (char)('0' + e / 10);
            out[n++] = (char)('0' + e % 10);
        }
    } else if (exp10 >= 0) {                     /* fixed, integer part present */
        for (i = 0; i <= exp10; i++) out[n++] = dig[i];
        last = 5; while (last > exp10 && dig[last] == '0') last--;
        if (last > exp10) { out[n++] = '.'; for (i = exp10 + 1; i <= last; i++) out[n++] = dig[i]; }
    } else {                                     /* fixed, 0.00ddd */
        last = 5; while (last > 0 && dig[last] == '0') last--;
        out[n++] = '0'; out[n++] = '.';
        for (i = 0; i < (-exp10 - 1); i++) out[n++] = '0';
        for (i = 0; i <= last; i++) out[n++] = dig[i];
    }
    return n;
}

void
io_write_real(int fd, double x)
{
    char buf[40];
    long f = (fd == 2) ? 2 : 1;

    kp_sys_write(f, buf, fmt_g(x, buf));
}

/* Scan a real: [sign] digits [. digits] [ (e|E) [sign] digits ], in double.
   Mirrors the sandbox's f32 scan_real, widened to the tier's f64 real. */
static double
pow10i(int e)
{
    double r = 1.0, b = 10.0;
    int n = e < 0 ? -e : e;

    while (n) {
        if (n & 1) r *= b;
        b *= b;
        n >>= 1;
    }
    return e < 0 ? 1.0 / r : r;
}

static double
scan_real(const char *b, int32_t len)
{
    int32_t i = 0, j, neg = 0, any = 0, expneg = 0, fracn = 0;
    int expval = 0, e;
    double m = 0.0, v;

    if (len == 0) return 0.0;
    if (b[i] == '+' || b[i] == '-') { neg = b[i] == '-'; i++; }
    while (i < len && b[i] >= '0' && b[i] <= '9') { m = m * 10.0 + (b[i] - '0'); i++; any = 1; }
    if (i < len && b[i] == '.') {
        i++;
        while (i < len && b[i] >= '0' && b[i] <= '9') { m = m * 10.0 + (b[i] - '0'); fracn++; i++; any = 1; }
    }
    if (!any) return 0.0;
    if (i < len && (b[i] == 'e' || b[i] == 'E')) {
        j = i + 1;
        if (j < len && (b[j] == '+' || b[j] == '-')) { expneg = b[j] == '-'; j++; }
        if (j < len && b[j] >= '0' && b[j] <= '9') {
            while (j < len && b[j] >= '0' && b[j] <= '9') {
                if (expval < 100000) expval = expval * 10 + (b[j] - '0');
                j++;
            }
            i = j;
        }
    }
    e = (expneg ? -expval : expval) - fracn;
    v = m * pow10i(e);
    return neg ? -v : v;
}

double
kp_str_val_real(uint32_t s)
{
    return scan_real(kp_str_bytes(s), kp_str_len(s));
}

double
io_read_real(void)
{
    char tok[64];
    int n = 0, c = skip_space();

    while (c != -1 && c != ' ' && c != '\t' && c != '\n' && c != '\r' && n < (int)sizeof tok)
        tok[n++] = (char)c, c = next_byte();
    if (c != -1)
        push_byte(c);
    return scan_real(tok, n);
}

/* The rv soft-float bridges: a double rides a call as its 64-bit bits.  Only the
   rv tier remaps the guest's real calls onto these; a target that passes a real
   in a float register (ColdFire) calls io_write_real / kp_str_val_real directly. */
#if (defined(__riscv) && __riscv_xlen == 32) || defined(__skj_riscv__)
void     brv_write_real(int fd, uint64_t x) { io_write_real(fd, b2d(x)); }
uint64_t brv_read_real(void)                { return d2b(io_read_real()); }
uint64_t brv_val_real(uint32_t s)           { return d2b(kp_str_val_real(s)); }
#endif

/****************************************************************
 * Program arguments (ParamCount / ParamStr over the entry argv)
 *
 * The crt hands the raw entry argument vector (argc then argv pointers) here at
 * startup through kp_set_raw_args; a host that passes none leaves the count 0.
 * ParamStr(0) is the program name, 1..count the arguments, as standard Pascal.
 ****************************************************************/

static int    g_argc;
static char **g_argv;

void
kp_set_raw_args(long *raw)
{
    if (raw == NULL) {
        g_argc = 0;
        g_argv = NULL;
        return;
    }
    g_argc = (int)raw[0];
    g_argv = (char **)&raw[1];
}

int
rt_param_count(void)
{
    return g_argc > 0 ? g_argc - 1 : 0;
}

uint32_t
rt_param_str(int i)
{
    const char *p;
    int n = 0;

    if (i < 0 || i >= g_argc || g_argv == NULL)
        return kp_str_new("", 0);
    p = g_argv[i];
    if (p == NULL)
        return kp_str_new("", 0);
    while (p[n])
        n++;
    return kp_str_new(p, n);
}

#ifdef KP_HAVE_FILE_IO
/****************************************************************
 * File I/O (ReadFileStr / WriteFileStr)
 *
 * A guest compiler such as kpcom reads its source file by path and writes its
 * output, so the freestanding tier needs file access the console seam does not
 * give.  The host open/read/close syscalls back these; skj-run reads at most a
 * page per read and has no lseek, so ReadFileStr grows a guest-heap buffer and
 * reads until EOF.  Both accept a NUL-terminated path copied out of the guest
 * string.  kp_sys_open / kp_sys_close are the per-target syscall wrappers above.
 ****************************************************************/

/* asm-generic open flags, shared by the guest and the host. */
#define KP_O_RDONLY  0
#define KP_O_WRONLY  1
#define KP_O_CREAT   0x40
#define KP_O_TRUNC   0x200

uint32_t
ReadFileStr(uint32_t pathstr)
{
    char path[1024];
    int plen = kp_str_len(pathstr);
    uint32_t cap = 262144, len = 0, buf;
    uint32_t s;
    int fd;
    long n;

    if (plen < 0 || plen >= (int)sizeof path)
        return kp_str_new("", 0);
    memcpy(path, kp_str_bytes(pathstr), (size_t)plen);
    path[plen] = '\0';

    fd = (int)kp_sys_open(path, KP_O_RDONLY, 0);
    if (fd < 0)
        return kp_str_new("", 0);

    buf = gheap_alloc(cap);
    for (;;) {
        if (len == cap) {
            cap *= 2;
            buf = gheap_realloc(buf, cap);
        }
        n = kp_sys_read(fd, (void *)(uintptr_t)(buf + len), (long)(cap - len));
        if (n <= 0)
            break;
        len += (uint32_t)n;
    }
    kp_sys_close(fd);

    s = kp_str_new_guest(buf, (int)len);
    gheap_free(buf);
    return s;
}

int
WriteFileStr(uint32_t pathstr, uint32_t datastr)
{
    char path[1024];
    int plen = kp_str_len(pathstr);
    uint32_t len;
    int fd;
    long n;

    if (plen < 0 || plen >= (int)sizeof path)
        return 0;
    memcpy(path, kp_str_bytes(pathstr), (size_t)plen);
    path[plen] = '\0';

    fd = (int)kp_sys_open(path, KP_O_WRONLY | KP_O_CREAT | KP_O_TRUNC, 0644);
    if (fd < 0)
        return 0;
    len = (uint32_t)kp_str_len(datastr);
    n = kp_sys_write(fd, kp_str_bytes(datastr), (long)len);
    kp_sys_close(fd);
    return n == (long)len;
}
#endif /* KP_HAVE_FILE_IO */
