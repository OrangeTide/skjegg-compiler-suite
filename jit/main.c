/* main.c : skj-jit, the in-process x86-64 JIT driver.

   Runs a C source through the cc front end to an ir_program, JITs it to
   native x86-64 with the jit/ library, and calls main() in-process.  No
   assembler, linker, or emulator is involved.  The library (jit_x86.c) is
   what a host would embed; this driver is a thin front end over it. */

#include "cc.h"
#include "cpp.h"
#include "arena.h"
#include "util.h"
#include "jit_x86.h"
#include "version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

/* Host runtime the guest reaches by name.  A guest pointer is a 32-bit
   address in the JIT's low-2GB mapping, zero-extended into a register, so it
   arrives here as a valid host pointer with no conversion. */
static long
host_write(int fd, const void *buf, unsigned long n)
{
    return (long)write(fd, buf, (size_t)n);
}

static long
host_read(int fd, void *buf, unsigned long n)
{
    return (long)read(fd, buf, (size_t)n);
}

static void
host_exit(int code)
{
    exit(code);
}

/* A fault the guest branches to instead of taking a hardware trap.  cc treats
   division by zero and signed overflow as errors here rather than the C
   standard's undefined behavior; the JIT emits the guard, this reports it. */
static void
host_trap(const char *what)
{
    fprintf(stderr, "skj-jit: runtime fault: %s\n", what);
    exit(70);
}

static void host_trap_div_zero(void) { host_trap("divide by zero"); }
static void host_trap_overflow(void) { host_trap("integer overflow"); }

/* 64-bit divide/modulo helpers.  skjegg's backends never grew native i64
   divide opcodes, so cc lowers a 64-bit divide to one of these libgcc-style
   calls.  Under X86_BITS=64 an i64 is a single native register and the guest
   passes them in the SysV registers, so a plain host division serves.  (The
   host is x86-64, where the compiler divides natively and does not itself pull
   __divdi3 from libgcc, so the driver provides them.) */
static long long h_divdi3(long long a, long long b) { return a / b; }
static long long h_moddi3(long long a, long long b) { return a % b; }
static unsigned long long h_udivdi3(unsigned long long a, unsigned long long b) { return a / b; }
static unsigned long long h_umoddi3(unsigned long long a, unsigned long long b) { return a % b; }

/* SysV va_arg helper (the __va_arg cc lowers each __builtin_va_arg to).  The
   guest is ILP32, so its va_list has 4-byte pointers and the four fields sit at
   byte offsets 0/4/8/12: gp_offset, fp_offset, overflow_arg_area,
   reg_save_area, the last two 32-bit low-memory addresses.  Read as raw words,
   mirroring runtime/va_x86_64.c.  Returns a pointer to the next argument slot
   and advances the va_list. */
static void *
h_va_arg(void *ap, int is_fp)
{
    unsigned *f = ap;
    unsigned p;
    if (is_fp) {
        if (f[1] < 176) { p = f[3] + f[1]; f[1] += 16; }
        else            { p = f[2]; f[2] += 8; }
    } else {
        if (f[0] < 48)  { p = f[3] + f[0]; f[0] += 8; }
        else            { p = f[2]; f[2] += 8; }
    }
    return (void *)(unsigned long)p;
}

/* A binding's address is a code pointer.  ISO C does not define a direct
   function-pointer-to-void* cast, so route it through a union, which keeps the
   build warning-clean under -Wpedantic. */
static void *
as_addr(void (*fn)(void))
{
    union { void (*f)(void); void *o; } u;
    u.f = fn;
    return u.o;
}

static struct kp_binding binds[16];
static int nbinds;

static void
bind(const char *name, void (*fn)(void))
{
    binds[nbinds].name = name;
    binds[nbinds].addr = as_addr(fn);
    nbinds++;
}

static void
cleanup_arena(void *p)
{
    arena_free(p);
}

static void
cleanup_cpp(void *p)
{
    cpp_free(p);
}

int
main(int argc, char **argv)
{
    const char *inpath;
    struct cpp *pp;
    char ppbuf[65536];
    int pplen;
    char linebuf[4096];
    int n;
    struct cc_node *ast;
    struct ir_program *prog;
    struct arena a;
    struct kp_jit j;
    char err[256];
    void *entry;
    int64_t iargs[6] = { 0 };
    double fargs[8] = { 0 };
    double fret;
    uintptr_t top;
    uint64_t result;

    util_set_progname("skj-jit");

    bind("write", (void (*)(void))host_write);
    bind("read", (void (*)(void))host_read);
    bind("exit", (void (*)(void))host_exit);
    bind("kp_trap_div_zero", host_trap_div_zero);
    bind("kp_trap_overflow", host_trap_overflow);
    bind("__divdi3", (void (*)(void))h_divdi3);
    bind("__moddi3", (void (*)(void))h_moddi3);
    bind("__udivdi3", (void (*)(void))h_udivdi3);
    bind("__umoddi3", (void (*)(void))h_umoddi3);
    bind("__va_arg", (void (*)(void))h_va_arg);

    arena_init(&a);
    pp = cpp_new();

    inpath = NULL;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "-I", 2) == 0) {
            const char *path = argv[i][2] ? &argv[i][2] : argv[++i];
            cpp_add_include_path(pp, path);
        } else if (strncmp(argv[i], "-D", 2) == 0) {
            const char *def = argv[i][2] ? &argv[i][2] : argv[++i];
            char *eq = strchr(def, '=');
            if (eq) {
                char name[256];
                int len = (int)(eq - def);
                if (len >= (int)sizeof(name)) len = (int)sizeof(name) - 1;
                memcpy(name, def, (size_t)len);
                name[len] = '\0';
                cpp_define(pp, name, eq + 1);
            } else {
                cpp_define(pp, def, "1");
            }
        } else if (strcmp(argv[i], "-V") == 0) {
            printf("skj-jit %s\n", SKJ_VERSION);
            cpp_free(pp);
            return 0;
        } else if (argv[i][0] == '-') {
            /* ignore unknown options silently */
        } else {
            inpath = argv[i];
        }
    }

    if (!inpath)
        die("usage: skj-jit [-V] [-I path] [-D name[=val]] input.c");

    if (setjmp(util_die_env) != 0)
        return 1;
    util_die_active = 1;

    util_cleanup_push(cleanup_arena, &a);
    util_cleanup_push(cleanup_cpp, pp);

    /* preprocess */
    pplen = 0;
    if (cpp_open(pp, inpath) < 0)
        die("cannot open '%s'", inpath);
    while ((n = cpp_next_line(pp, linebuf, sizeof linebuf)) > 0) {
        if (pplen + n >= (int)sizeof(ppbuf) - 1)
            die("preprocessed source too large");
        memcpy(ppbuf + pplen, linebuf, (size_t)n);
        pplen += n;
    }
    ppbuf[pplen] = '\0';
    cpp_free(pp);
    util_cleanup_pop();

    /* parse and lower to IR */
    cc_lex_init(&a, ppbuf, inpath);
    ast = cc_parse_program(&a);
    prog = cc_lower_program(&a, ast);

    /* JIT the whole program (kp_jit runs the register allocator itself) */
    if (kp_jit(&j, prog, binds, nbinds, err, sizeof err) != 0)
        die("jit: %s", err);

    entry = kp_jit_entry(&j, "main");
    if (!entry)
        die("no 'main' in the program");

    /* run main() on the JIT's low-2GB stack and take its int return as the
       process exit status.  main(void) reads no arguments; the zeroed arg
       arrays are harmless if it declares (argc, argv). */
    top = ((uintptr_t)j.stack + j.stack_len) & ~(uintptr_t)15;
    result = kp_call_guest(entry, (void *)top, iargs, fargs, &fret);

    kp_jit_free(&j);
    util_cleanup_run();
    return (int)result;
}
