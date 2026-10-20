/* io_free.c : the freestanding console and number surface for a Kobold guest.

   The re-bottoming of kp_host.c for a host whose only output seam is the write
   syscall and whose only input seam is read.  kp_host.c backs these on stdio
   (fprintf, fgetc); here they go straight to the seam, with no libc.  Integer Str
   and Val stay hand-rolled here (the same approach as the upstream sandbox):
   Pascal's Val carries a '$' hex prefix and 1-based error-offset codes that C's
   strtol does not model.  Real parsing, a plain decimal-float scan, is shared
   instead: scan_real routes to libc's strtod, so this runtime and a C program
   built against the same libc parse a real the one way.  Real formatting (fmt_g)
   is still hand-rolled to avoid the double-to-u64 helper libc's %g path pulls,
   which the rv32 freestanding tier has no libgcc for.

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
   (runtime/io_free_cf.S, runtime/io_free_rv.S).  Both targets provide
   open/close/lseek as well as write/read/exit, so both enable the file-I/O
   surface at the end of this file. */
#if (defined(__riscv) && __riscv_xlen == 32) || defined(__skj_riscv__)
#define KP_HAVE_FILE_IO 1
#elif defined(__mcoldfire__) || defined(__skj_m68k__)
#define KP_HAVE_FILE_IO 1
#else
#error "io_free.c is a freestanding console; build it with the rv32 or ColdFire toolchain"
#endif

/* The unified syscall seam: the same POSIX-named wrappers the C libc links
   (runtime/start_rv.S / start.S for a C program; runtime/io_free_*.S for this
   freestanding runtime).  write/read return the count or -errno; _exit ends the
   process. */
int write(int fd, const void *buf, int len);
int read(int fd, void *buf, int len);
void _exit(int code);
#ifdef KP_HAVE_FILE_IO
int open(const char *path, int flags, int mode);
int close(int fd);
int getdents64(int fd, void *buf, int count);
#endif

/****************************************************************
 * The buffered-stream seam: libc's stdio_core.c
 *
 * The console I/O below buffers through libc's standard streams rather than
 * hitting the write/read seam a byte at a time, so the Kobold freestanding
 * runtime and the C runtime share one buffering implementation.  Declared
 * opaquely here so this freestanding file needs no -Iinclude; keep in step with
 * include/stdio.h.
 ****************************************************************/
struct __FILE;
extern struct __FILE __libc_stdin, __libc_stdout, __libc_stderr;
extern int fputc(int c, struct __FILE *f);
extern int fgetc(struct __FILE *f);
extern size_t fwrite(const void *p, size_t sz, size_t n, struct __FILE *f);
extern int fflush(struct __FILE *f);

/****************************************************************
 * Console output (the io_* names the guest emits)
 ****************************************************************/

static struct __FILE *
io_stream(int fd)
{
    return (fd == 2) ? &__libc_stderr : &__libc_stdout;
}

void
io_write_str(int fd, uint32_t s)
{
    int32_t len = kp_str_len(s);

    if (len > 0)
        fwrite((const void *)(uintptr_t)kp_str_base(s), 1, (size_t)len, io_stream(fd));
}

void
io_write_char(int fd, int ch)
{
    fputc((unsigned char)ch, io_stream(fd));
}

/* A decimal integer formatted into a stack buffer, no heap string.  INT_MIN is
   safe: the magnitude is taken in unsigned. */
void
io_write_int(int fd, int v)
{
    char buf[12];
    int n = 0;
    uint32_t m = v < 0 ? 0u - (uint32_t)v : (uint32_t)v;
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
    fwrite(out, 1, (size_t)j, io_stream(fd));
}

/* A real flush now; halt flushes every stream, then exits (buffered output must
   reach the seam before the process ends). */
void io_flush(int fd) { fflush(io_stream(fd)); }
void io_halt(int code) { fflush((struct __FILE *)0); _exit(code); }

/****************************************************************
 * Console input (over the read syscall, one byte at a time)
 ****************************************************************/

static int
read_byte(void)
{
    return fgetc(&__libc_stdin);        /* libc buffering; EOF (-1) at end */
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
 * strtod (kp_host.c / str_fmt.c).  This tier parses with the same strtod (shared
 * from libc, see scan_real below) but hand-rolls a %g matching formatter (six
 * significant digits, trailing zeros stripped), because libc's %g path pulls a
 * double-to-u64 helper the rv32 freestanding tier has no libgcc for.  The rv32
 * soft-float ABI carries a double across a call as its
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

    fwrite(buf, 1, (size_t)fmt_g(x, buf), io_stream(fd));
}

/* Scan a real: the freestanding tier parses it with libc's strtod
   (libc/strtod.c), the same decimal-float parser a C program built against this
   libc uses, so the two runtimes share one real parser.  strtod wants a
   NUL-terminated string while this tier's callers pass a counted buffer, so copy
   the (bounded) token into a local NUL-terminated scratch first: a real literal
   never needs more than a few dozen characters, and a double cannot resolve more
   digits than this buffer holds. */
extern double strtod(const char *s, char **end);

static double
scan_real(const char *b, int32_t len)
{
    char buf[64];
    int32_t n = len < 0 ? 0 : (len > 63 ? 63 : len);

    memcpy(buf, b, (size_t)n);
    buf[n] = '\0';
    return strtod(buf, (char **)0);
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
 * string.  open / close are the per-target syscall wrappers above.
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

    fd = (int)open(path, KP_O_RDONLY, 0);
    if (fd < 0)
        return kp_str_new("", 0);

    buf = gheap_alloc(cap);
    for (;;) {
        if (len == cap) {
            cap *= 2;
            buf = gheap_realloc(buf, cap);
        }
        n = read(fd, (void *)(uintptr_t)(buf + len), (long)(cap - len));
        if (n <= 0)
            break;
        len += (uint32_t)n;
    }
    close(fd);

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

    fd = (int)open(path, KP_O_WRONLY | KP_O_CREAT | KP_O_TRUNC, 0644);
    if (fd < 0)
        return 0;
    len = (uint32_t)kp_str_len(datastr);
    n = write(fd, kp_str_bytes(datastr), (long)len);
    close(fd);
    return n == (long)len;
}

/* The line/stream file primitives the `files` unit builds on: open a handle,
   read one chunk, write bytes, close.  Stateless (the unit holds the fd and does
   its own line buffering), mirroring jit_main.c and aot_rt.c over this tier's
   open/read/write/close syscall seam. */
#define KPF_CHUNK 4096

int
kpf_open_read(uint32_t pathstr)
{
    char path[1024];
    int plen = kp_str_len(pathstr);

    if (plen < 0 || plen >= (int)sizeof path)
        return -1;
    memcpy(path, kp_str_bytes(pathstr), (size_t)plen);
    path[plen] = '\0';
    return (int)open(path, KP_O_RDONLY, 0);
}

int
kpf_open_write(uint32_t pathstr)
{
    char path[1024];
    int plen = kp_str_len(pathstr);

    if (plen < 0 || plen >= (int)sizeof path)
        return -1;
    memcpy(path, kp_str_bytes(pathstr), (size_t)plen);
    path[plen] = '\0';
    return (int)open(path, KP_O_WRONLY | KP_O_CREAT | KP_O_TRUNC, 0644);
}

uint32_t
kpf_read(int fd)
{
    char tmp[KPF_CHUNK];
    long n = (fd < 0) ? -1 : read(fd, tmp, (long)sizeof tmp);

    if (n <= 0)
        return kp_str_new("", 0);
    return kp_str_new(tmp, (int)n);
}

int
kpf_write(int fd, uint32_t datastr)
{
    if (fd < 0)
        return -1;
    return (int)write(fd, kp_str_bytes(datastr), (long)kp_str_len(datastr));
}

void
kpf_close(int fd)
{
    if (fd >= 0)
        close(fd);
}

/* Directory reads for the `dirs` unit.  A small fixed table of open
   directories: a Kobold handle is a table index, the walk over the raw
   getdents64 buffer is done here so the unit sees one name at a time.  No
   malloc in the freestanding runtime, so the per-handle buffer is in the
   table. */
#define KPD_MAX 8
#define KPD_BUF 2048
static struct {
    int fd;
    int inuse;
    int pos;                    /* next unread byte in buf */
    int len;                    /* valid bytes in buf */
    char buf[KPD_BUF];
} kpd_tab[KPD_MAX];

int
kpd_open(uint32_t pathstr)
{
    char path[1024];
    int plen = kp_str_len(pathstr);
    int i, fd;

    if (plen < 0 || plen >= (int)sizeof path)
        return -1;
    memcpy(path, kp_str_bytes(pathstr), (size_t)plen);
    path[plen] = '\0';
    for (i = 0; i < KPD_MAX; i++)
        if (!kpd_tab[i].inuse)
            break;
    if (i == KPD_MAX)
        return -1;
    fd = (int)open(path, KP_O_RDONLY, 0);
    if (fd < 0)
        return -1;
    kpd_tab[i].fd = fd;
    kpd_tab[i].inuse = 1;
    kpd_tab[i].pos = 0;
    kpd_tab[i].len = 0;
    return i;
}

uint32_t
kpd_next(int h)
{
    char *e;
    unsigned short reclen;
    const char *name;
    int nl;

    if (h < 0 || h >= KPD_MAX || !kpd_tab[h].inuse)
        return kp_str_new("", 0);
    if (kpd_tab[h].pos >= kpd_tab[h].len) {
        int n = getdents64(kpd_tab[h].fd, kpd_tab[h].buf, KPD_BUF);
        if (n <= 0)
            return kp_str_new("", 0);
        kpd_tab[h].len = n;
        kpd_tab[h].pos = 0;
    }
    e = kpd_tab[h].buf + kpd_tab[h].pos;
    memcpy(&reclen, e + 16, sizeof reclen);   /* struct linux_dirent64 d_reclen */
    if (reclen < 20) {                        /* guard a malformed entry */
        kpd_tab[h].pos = kpd_tab[h].len;
        return kp_str_new("", 0);
    }
    kpd_tab[h].pos += reclen;
    name = e + 19;                            /* d_name, NUL-terminated */
    nl = 0;
    while (nl < reclen - 19 && name[nl])
        nl++;
    return kp_str_new(name, nl);
}

void
kpd_close(int h)
{
    if (h >= 0 && h < KPD_MAX && kpd_tab[h].inuse) {
        close(kpd_tab[h].fd);
        kpd_tab[h].inuse = 0;
    }
}
#endif /* KP_HAVE_FILE_IO */
