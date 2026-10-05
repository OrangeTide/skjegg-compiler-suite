/* kp_host.c : the standard Kobold runtime for the Skjegg JIT (see kp_host.h).

   Every routine here is a direct host function matching the guest's register ABI
   (arguments in the integer/float argument registers), which is how Skjegg's JIT
   binds a host symbol.  This is unlike Kobold's own value-stack host ABI, which
   load.c bridges with a generated thunk; here the front end's lowered calls land
   on these C functions directly.  The I/O and number routines mirror src/rt.c and
   src/str_fmt.c so behavior matches the Kobold VM byte for byte. */

#include "kp_host.h"
#include "str.h"
#include "dynarr.h"
#include "map.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <math.h>

/* Coroutine operators, provided vm-lessly by kp/co_shim.c over co.c. */
uint32_t kp_co_new_g(void *entry, const void *args, int nargs);
int64_t kp_co_resume_g(uint32_t co, int64_t value);
int64_t kp_co_yield_g(int64_t value);
uint32_t kp_co_self_g(void);
int kp_co_cancelling_g(void);

/****************************************************************
 * Console I/O (mirrors src/rt.c)
 ****************************************************************/

static FILE *fd_stream(int fd) { return fd == 2 ? stderr : stdout; }

/* A guest string address is a real low-memory pointer, so the ported runtime
   reads it directly through kp_str_bytes/kp_str_len. */
void io_write_str(int fd, uint32_t sp)
{
    fwrite(kp_str_bytes(sp), 1, (size_t)kp_str_len(sp), fd_stream(fd));
}
void io_write_int(int fd, int v) { fprintf(fd_stream(fd), "%d", v); }
void io_write_char(int fd, int ch) { fputc(ch, fd_stream(fd)); }
void io_write_ln(int fd) { fputc('\n', fd_stream(fd)); }
void io_flush(int fd) { fflush(fd_stream(fd)); }
void io_halt(int code) { exit(code); }

/* `real` is f64, written with a plain "%g" (the Kobold dialect has no field-width
   formatting), matching src/rt.c io_write_real. */
void io_write_real(int fd, double x) { fprintf(fd_stream(fd), "%g", x); }

static int io_skip_space(void)
{
    int ch;
    do {
        ch = getchar();
    } while (ch != EOF && isspace((unsigned char)ch));
    return ch;
}

/* Returns a 64-bit value: the front end reads an int64 with the same import,
   calling it as CALL64 to take the full register, while a plain int read uses the
   low half.  A narrower return would truncate an int64 like 5000000000. */
int64_t io_read_int(void)
{
    int ch = io_skip_space();
    int neg = 0;
    long long v = 0;

    if (ch == '+' || ch == '-') {
        neg = (ch == '-');
        ch = getchar();
    }
    while (ch >= '0' && ch <= '9') {
        v = v * 10 + (ch - '0');
        ch = getchar();
    }
    if (ch != EOF)
        ungetc(ch, stdin);
    return (int64_t)(neg ? -v : v);
}

double io_read_real(void)
{
    char buf[64];
    int n = 0;
    int ch = io_skip_space();

    while (ch != EOF && !isspace((unsigned char)ch) && n < (int)sizeof(buf) - 1) {
        buf[n++] = (char)ch;
        ch = getchar();
    }
    if (ch != EOF)
        ungetc(ch, stdin);
    buf[n] = '\0';
    return n ? strtod(buf, NULL) : 0.0;
}

int io_read_char(void)
{
    int ch = getchar();
    return ch == EOF ? 0 : (unsigned char)ch;
}

void io_read_ln(void)
{
    int ch;
    while ((ch = getchar()) != EOF && ch != '\n')
        ;
}

/* Read a line (not consuming the newline) into a fresh guest string (owned,
   refcount 1), or 0 for the empty line.  Mirrors src/rt.c io_read_str. */
uint32_t io_read_str(void)
{
    char *buf = NULL;
    size_t cap = 0, n = 0;
    int ch;
    uint32_t s;

    while ((ch = getchar()) != EOF && ch != '\n') {
        if (n + 1 > cap) {
            size_t ncap = cap ? cap * 2 : 64;
            char *grown = realloc(buf, ncap);
            if (!grown)
                break;
            buf = grown;
            cap = ncap;
        }
        buf[n++] = (char)ch;
    }
    if (ch == '\n')
        ungetc(ch, stdin);

    s = kp_str_new(buf ? buf : "", (int)n);
    free(buf);
    return s;
}

/****************************************************************
 * Float ABI bridge (rv32 only)
 ****************************************************************/

/* The rv JIT passes and returns a `double` across a call in an integer register
   pair: that is its soft-float (ilp32) call ABI, shared with the AOT path.  The
   hosted libc, however, is ilp32d (a double rides in fa0), so a double handed
   straight to a libc-compiled host routine is read from the wrong place.  These
   wrappers carry the double as its uint64_t bits instead, which occupy an integer
   register pair under either float ABI and so match the JIT.  Every other target
   passes a double in its native float register, matching the JIT already, so the
   runtime routines are bound directly there. */
#if defined(__riscv) && __riscv_xlen == 32
#define KP_RV_FLOAT_BRIDGE 1

static double b2d(uint64_t b) { double d; memcpy(&d, &b, sizeof d); return d; }
static uint64_t d2b(double d) { uint64_t b; memcpy(&b, &d, sizeof b); return b; }

/* These carry the guest's import names at link in the AOT path (aot_main.c
   remaps the guest call target to them), so they are global, not static; the JIT
   reaches them by address through its binding table either way. */
void     brv_write_real(int fd, uint64_t x) { io_write_real(fd, b2d(x)); }
uint64_t brv_read_real(void)                { return d2b(io_read_real()); }
uint64_t brv_val_real(uint32_t s)           { return d2b(kp_str_val_real(s)); }

uint64_t brv_sin(uint64_t x)    { return d2b(sin(b2d(x))); }
uint64_t brv_cos(uint64_t x)    { return d2b(cos(b2d(x))); }
uint64_t brv_exp(uint64_t x)    { return d2b(exp(b2d(x))); }
uint64_t brv_ln(uint64_t x)     { return d2b(log(b2d(x))); }
uint64_t brv_arctan(uint64_t x) { return d2b(atan(b2d(x))); }
#else
/* Transcendentals: the front end lowers sin/cos/exp/ln/arctan to a call taking
   one double and returning a double in the register float ABI, so these wrap the
   C library directly (the rv32 soft-float ABI uses the bridged forms above).
   Global for the AOT link path, as the rv bridges above. */
double h_sin(double x)    { return sin(x); }
double h_cos(double x)    { return cos(x); }
double h_exp(double x)    { return exp(x); }
double h_ln(double x)     { return log(x); }
double h_arctan(double x) { return atan(x); }
#endif

/****************************************************************
 * Faults and 64-bit helpers
 ****************************************************************/

static void host_trap(const char *what)
{
    fprintf(stderr, "skj-kp: runtime fault: %s\n", what);
    exit(70);
}
static void host_trap_div_zero(void) { host_trap("divide by zero"); }
static void host_trap_overflow(void) { host_trap("integer overflow"); }
static void host_trap_index(void) { host_trap("array index out of bounds"); }

/* On a host where an i64 is a single register (x86-64) or follows the C
   register ABI (rv32), a plain host division serves the __divdi3-family names the
   front end lowers to. */
static long long h_divdi3(long long a, long long b) { return a / b; }
static long long h_moddi3(long long a, long long b) { return a % b; }
static unsigned long long h_udivdi3(unsigned long long a, unsigned long long b) { return a / b; }
static unsigned long long h_umoddi3(unsigned long long a, unsigned long long b) { return a % b; }

/* RV32 has no native 64-bit register, so the backend lowers a 64-bit multiply and
   the shift family to stack-ABI helpers (jit/rv_jit_i64.S), distinct from the
   register-ABI __divdi3 above.  They carry skj_jit_* names so they do not collide
   with the driver's own libgcc; the guest reaches them by their __*di3 names,
   bound below.  Other targets do these inline and never emit the calls. */
#if defined(__riscv) && __riscv_xlen == 32
extern void skj_jit_muldi3(void);
extern void skj_jit_ashldi3(void);
extern void skj_jit_ashrdi3(void);
extern void skj_jit_lshrdi3(void);
#endif

/****************************************************************
 * Command-line arguments (ParamStr / ParamCount)
 ****************************************************************/

/* The command line, as recorded by kp_io_set_args.  g_args[0] is the module name
   (standing in for the program name) and g_args[1..] are the program's own args. */
static int    g_nargs;
static char **g_args;

void
kp_io_set_args(int argc, char **argv)
{
    /* skip the host launcher's own name (argv[0]); the module name leads the rest */
    if (argc > 1) {
        g_nargs = argc - 1;
        g_args = argv + 1;
    } else {
        g_nargs = 0;
        g_args = NULL;
    }
}

/* The number of command-line arguments after the program name.  Global so the
   AOT link resolves the guest's ParamCount import against it (aot_main.c remap). */
int rt_param_count(void)
{
    return g_nargs > 0 ? g_nargs - 1 : 0;
}

/* The i-th command-line argument as a new guest string, empty when out of range.
   Argument 0 is the program (module) name.  Global for the AOT link path. */
uint32_t rt_param_str(int32_t i)
{
    const char *s = (i >= 0 && i < g_nargs) ? g_args[i] : "";
    return kp_str_new(s, (int)strlen(s));
}

/****************************************************************
 * Binding helpers
 ****************************************************************/

/* ISO C does not define a direct function-pointer-to-void* cast; route it through
   a union to stay warning-clean under -Wpedantic. */
static void *as_addr(void (*fn)(void))
{
    union { void (*f)(void); void *o; } u;
    u.f = fn;
    return u.o;
}

void
kp_install_runtime(struct kp_binding *binds, int *n, int cap)
{
    int i = *n;

#define BIND(nm, f)                                     \
    do {                                                \
        if (i < cap) {                                  \
            binds[i].name = (nm);                       \
            binds[i].addr = as_addr((void (*)(void))(f)); \
            i++;                                        \
        }                                               \
    } while (0)

    BIND("io_write_str", io_write_str);
    BIND("io_write_int", io_write_int);
    BIND("io_write_char", io_write_char);
    BIND("io_write_ln", io_write_ln);
    BIND("io_flush", io_flush);
    BIND("io_read_int", io_read_int);
    BIND("io_read_char", io_read_char);
#ifdef KP_RV_FLOAT_BRIDGE
    BIND("io_write_real", brv_write_real);
    BIND("io_read_real", brv_read_real);
#else
    BIND("io_write_real", io_write_real);
    BIND("io_read_real", io_read_real);
#endif
    BIND("io_read_str", io_read_str);
    BIND("io_read_ln", io_read_ln);
    BIND("io_halt", io_halt);

    BIND("ParamCount", rt_param_count);
    BIND("ParamStr", rt_param_str);
    BIND("heapLive", kp_gheap_live);

    BIND("kp_retain", kp_retain);
    BIND("kp_release", kp_release);
    BIND("kp_trap_index", host_trap_index);
    BIND("kp_trap_div_zero", host_trap_div_zero);
    BIND("kp_trap_overflow", host_trap_overflow);

    BIND("kp_str_concat", kp_str_concat);
    BIND("kp_str_len", kp_str_len);
    BIND("kp_str_base", kp_str_base);
    BIND("kp_str_cmp", kp_str_cmp);
    BIND("kp_str_pos", kp_str_pos);
    BIND("kp_str_copy", kp_str_copy);
    BIND("kp_str_from_int", kp_str_from_int);
    BIND("kp_str_from_int64", kp_str_from_int64);
    BIND("kp_str_from_char", kp_str_from_char);
    BIND("kp_str_val_int", kp_str_val_int);
    BIND("kp_str_val_int64", kp_str_val_int64);
    BIND("kp_str_valcode", kp_str_valcode);
#ifdef KP_RV_FLOAT_BRIDGE
    BIND("kp_str_val_real", brv_val_real);
#else
    BIND("kp_str_val_real", kp_str_val_real);
#endif

    BIND("kp_arr_len", kp_arr_len);
    BIND("kp_arr_base", kp_arr_base);
    BIND("kp_arr_setlen", kp_arr_setlen);
    BIND("kp_arr_clone", kp_arr_clone);
    BIND("kp_arr_delete", kp_arr_delete);
    BIND("kp_arr_insert", kp_arr_insert);

    BIND("kp_map_new", kp_map_new);
    BIND("kp_map_reserve", kp_map_reserve);
    BIND("kp_map_get", kp_map_get);
    BIND("kp_map_tryget", kp_map_tryget);
    BIND("kp_map_has", kp_map_has);
    BIND("kp_map_del", kp_map_del);
    BIND("kp_map_len", kp_map_len);
    BIND("kp_map_clone", kp_map_clone);
    BIND("kp_mapiter_new", kp_mapiter_new);
    BIND("kp_mapiter_next", kp_mapiter_next);

#ifdef KP_RV_FLOAT_BRIDGE
    BIND("kp_sin", brv_sin);
    BIND("kp_cos", brv_cos);
    BIND("kp_exp", brv_exp);
    BIND("kp_ln", brv_ln);
    BIND("kp_arctan", brv_arctan);
#else
    BIND("kp_sin", h_sin);
    BIND("kp_cos", h_cos);
    BIND("kp_exp", h_exp);
    BIND("kp_ln", h_ln);
    BIND("kp_arctan", h_arctan);
#endif

    BIND("kp_co_new", kp_co_new_g);
    BIND("kp_co_resume", kp_co_resume_g);
    BIND("kp_co_yield", kp_co_yield_g);
    BIND("kp_co_self", kp_co_self_g);
    BIND("kp_co_cancelling", kp_co_cancelling_g);

    BIND("__divdi3", h_divdi3);
    BIND("__moddi3", h_moddi3);
    BIND("__udivdi3", h_udivdi3);
    BIND("__umoddi3", h_umoddi3);

#if defined(__riscv) && __riscv_xlen == 32
    BIND("__muldi3", skj_jit_muldi3);
    BIND("__ashldi3", skj_jit_ashldi3);
    BIND("__ashrdi3", skj_jit_ashrdi3);
    BIND("__lshrdi3", skj_jit_lshrdi3);
#endif

#undef BIND
    *n = i;
}

char *
kp_read_file(const char *path, size_t *lenp)
{
    FILE *f = fopen(path, "rb");
    long sz;
    char *buf;

    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[sz] = '\0';
    *lenp = (size_t)sz;
    return buf;
}
