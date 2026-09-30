/* exc_main.c : skj-exc-jit-arm64, the in-process Excelsior JIT driver.

   Runs an Excelsior source through the exc front end to an ir_program, JITs it
   to native AArch64 with the jit/ library, links the portable guest runtime
   (runtime/libexc.c built EXC_GUEST32, as LP64 host code over the 32-bit guest
   object model), and runs the entry verb in-process.  No assembler, linker, or
   emulator is involved.

   This driver is the JIT counterpart of runtime/exc_native.c: it owns the
   __exh_* binding surface and the entry bootstrap.  The runtime primitives the
   guest calls (__exc_send, __exc_spawn, the value helpers) are the same host
   libexc functions the driver itself calls; the guest reaches them by name
   through the binding table, resolved with dlsym over the driver's own dynamic
   symbols (the driver is linked -rdynamic).  __exc_self is a host libexc
   variable the guest also reads, so it rides the same table as a data symbol;
   the driver is linked non-PIE so its address is below 4GB, which the guest
   loads into a 32-bit register. */

#include "excelsior.h"
#include "ir.h"
#include "arena.h"
#include "util.h"
#include "jit_common.h"
#include "libexc.h"
#include "version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <dlfcn.h>

/* ---- the host binding surface libexc calls directly (host-abi.md D5) ---- */

/* Channel 0 is the author channel (trace and fault text, stderr); channel 1 is
   the player console (stdout).  buf is a guest pointer, which arrives here as a
   valid host pointer because the guest arena is mapped below 4GB. */
void
__exh_emit(long chan, const char *buf, long len)
{
    ssize_t r = write(chan == 0 ? 2 : 1, buf, (size_t)len);
    (void)r;
}

/* libexc has already emitted the fault report on channel 0; the native routing
   is the classic exit 70 (runtime-errors.md). */
void
__exh_fault(const struct exc_trapdesc *why, long a, long b)
{
    (void)why; (void)a; (void)b;
    exit(70);
}

/* The native tier has no persistence, no other VMs, and no scheduler. */
long __exh_prop_get(long o, const char *k, char *b, long n) { (void)o; (void)k; (void)b; (void)n; return -1; }
long __exh_prop_put(long o, const char *k, const char *v) { (void)o; (void)k; (void)v; return -1; }
long __exh_post(long t, const char *b, long n) { (void)t; (void)b; (void)n; return -1; }
void __exh_yield(void) {}
void __exh_sleep(long ms) { (void)ms; }

/* The host __exc_selnames libexc's exc_sel_by_name links against.  The guest's
   own selector table is a program global the driver reads directly (below), so
   this host copy is only here to satisfy the link and stays empty. */
word __exc_selnames[1] = { 0 };

/* The backend's guarded-trap targets (ARM64_TRAP_GUARDED): the JIT cannot let a
   hardware divide fault fire in-process, so the backend guards a divide or an
   overflowing negate and calls one of these instead.  Excelsior's own fallible
   faults route through __exc_trap; these catch the backend-level cases (a
   nonconsumable divide the backend must guard, INT_MIN / -1).  Both are the
   fault exit, 70. */
void kp_trap_div_zero(void) { exit(70); }
void kp_trap_overflow(void) { exit(70); }

/* The console: a native player object dispatched through the ordinary
   __exc_send path.  tell(msg) writes the string plus a newline to stdout. */
static word
console_tell(struct exc_str *msg)
{
    if (msg && msg->len)
        __exh_emit(1, EXC_SDATA(msg), msg->len);
    __exh_emit(1, "\n", 1);
    return 0;
}

/* ---- the guest's runtime imports, bound by name ---- */

/* Every runtime primitive the compiler emits a call to.  Each is a host libexc
   symbol, resolved with dlsym; a name that is not linked in this build (the
   coroutine switch, still to come) is simply left unbound, and only a
   source-using program would reach it. */
static const char *const runtime_syms[] = {
    "__exc_send", "__exc_spawn", "__exc_box", "__exc_trap",
    "kp_trap_div_zero", "kp_trap_overflow",
    "__exc_decmul", "__exc_decdiv",
    "__exc_str_concat", "__exc_str_eq", "__exc_str_cmp", "__exc_str_len",
    "__exc_str_at", "__exc_str_slice", "__exc_str_find",
    "__exc_str_from_int", "__exc_str_from_float", "__exc_str_from_dec",
    "__exc_str_from_bool", "__exc_str_from_enum",
    "__exc_list_concat", "__exc_list_append", "__exc_list_prepend",
    "__exc_list_insert", "__exc_list_set", "__exc_list_delete",
    "__exc_list_reverse", "__exc_list_slice", "__exc_list_contains",
    "__exc_list_contains_str", "__exc_list_contains_rec",
    "__exc_list_map", "__exc_list_filter", "__exc_list_reduce", "__exc_list_sort",
    "__exc_rec_new", "__exc_rec_copy", "__exc_rec_blit", "__exc_rec_eq",
    "__exc_trace_int", "__exc_trace_float", "__exc_trace_dec",
    "__exc_trace_bool", "__exc_trace_str", "__exc_trace_nomatch",
    "__exc_src_new", "__exc_src_next", "__exc_src_yield",
    "__moo_arena_alloc",
    /* __exc_self is a data symbol, not a call, but resolves the same way */
    "__exc_self",
};

static struct kp_binding binds[64];
static int nbinds;

static void
bind_runtime(void)
{
    size_t n = sizeof(runtime_syms) / sizeof(runtime_syms[0]);
    for (size_t i = 0; i < n && nbinds < (int)(sizeof(binds) / sizeof(binds[0])); i++) {
        void *a = dlsym(RTLD_DEFAULT, runtime_syms[i]);
        if (a) {
            binds[nbinds].name = runtime_syms[i];
            binds[nbinds].addr = a;
            nbinds++;
        }
    }
}

static void
cleanup_arena(void *p)
{
    arena_free(p);
}

static void
cleanup_free(void *p)
{
    free(p);
}

/* ISO C does not define a function-pointer-to-void* cast; route it through a
   union so kp_call_guest can take __exc_send as its entry, warning-clean under
   -Wpedantic. */
static void *
as_obj(word (*fn)(struct exc_obj *, long, long, word *))
{
    union { word (*f)(struct exc_obj *, long, long, word *); void *o; } u;
    u.f = fn;
    return u.o;
}

/* Read a compiler-emitted entry global (a single guest word) from the JIT's
   data region. */
static uint32_t
entry_word(struct kp_jit *j, const char *name)
{
    uint8_t *p = kp_global_addr(j, name);
    uint32_t v = 0;
    if (p)
        memcpy(&v, p, sizeof v);
    return v;
}

int
main(int argc, char **argv)
{
    const char *volatile inpath = NULL;   /* read after setjmp; volatile avoids -Wclobbered */
    volatile int trace = 0;
    char *src;
    struct node *ast;
    struct ir_program *prog;
    struct arena a;
    struct kp_jit j;
    char err[256];

    util_set_progname("skj-exc-jit-arm64");

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-V") == 0) {
            printf("skj-exc-jit-arm64 %s\n", SKJ_VERSION);
            return 0;
        } else if (strcmp(argv[i], "-t") == 0) {
            trace = 1;
        } else if (argv[i][0] == '-') {
            /* ignore unknown options */
        } else {
            inpath = argv[i];
        }
    }
    if (!inpath)
        die("usage: skj-exc-jit-arm64 [-V] [-t] input.exs");

    if (setjmp(util_die_env) != 0)
        return 1;
    util_die_active = 1;

    src = slurp(inpath);
    util_cleanup_push(cleanup_free, src);
    arena_init(&a);
    util_cleanup_push(cleanup_arena, &a);

    lex_init(&a, src, inpath, trace);
    ast = parse_program(&a);
    resolve_program(&a, ast);
    typecheck_program(&a, ast);
    prog = lower_program(&a, ast);

    bind_runtime();
    if (kp_jit(&j, prog, binds, nbinds, err, sizeof err) != 0)
        die("jit: %s", err);

    {
        /* bootstrap like exc_native.c's main(), but read the entry symbols
           from the JIT's data region rather than through host linkage */
        uint32_t entry_class = entry_word(&j, "__exc_entry_class");
        long selector = (int32_t)entry_word(&j, "__exc_entry_selector");
        long want_argc = (int32_t)entry_word(&j, "__exc_entry_argc");
        struct class_desc *ecls = (struct class_desc *)(uintptr_t)entry_class;
        struct exc_obj *bootstrap;
        struct class_desc *cdesc;
        struct exc_obj *console;
        word cargv[1];
        long cargc = 0;
        long tell = -1;
        int64_t iargs[6] = { 0 };
        double fargs[8] = { 0 };
        double fret = 0;
        uintptr_t top;
        uint64_t result;

        /* resolve `tell` against the guest's selector table (a program
           global { count, &name0, ... }) and wire the console descriptor */
        {
            uint8_t *snp = kp_global_addr(&j, "__exc_selnames");
            if (snp) {
                uint32_t count, e;
                memcpy(&count, snp, 4);
                for (uint32_t i = 0; i < count; i++) {
                    memcpy(&e, snp + 4 + 4 * i, 4);
                    if (strcmp((const char *)(uintptr_t)e, "tell") == 0) {
                        tell = i;
                        break;
                    }
                }
            }
        }

        cdesc = __moo_arena_alloc((int)(sizeof(struct class_desc) + 2 * sizeof(word)));
        console = __moo_arena_alloc((int)sizeof(struct exc_obj));
        if (!cdesc || !console)
            die("jit: runtime arena exhausted at bootstrap");
        cdesc->parent = EXC_TOG((void *)0);
        cdesc->nverbs = (tell >= 0) ? 1 : 0;
        cdesc->nwords = 0;
        cdesc->image = EXC_TOG((void *)0);
        if (tell >= 0) {
            cdesc->verbs[0] = (word)tell;
            cdesc->verbs[1] = (word)(uintptr_t)&console_tell;
        }
        console->cls = EXC_TOG(cdesc);

        if (!ecls)
            die("jit: no entry class in the program");
        bootstrap = __exc_spawn(ecls);
        __exc_self = EXC_TOG(bootstrap);
        if (want_argc >= 1) {
            cargv[0] = (word)(uintptr_t)console;
            cargc = 1;
        }

        /* Run __exc_send on the JIT's low-memory execution stack, so the verb
           it dispatches (and everything it sends on) has 32-bit frame
           addresses.  __exc_send is a host function; kp_call_guest just puts
           its four integer arguments in registers and switches the stack. */
        iargs[0] = (int64_t)(uintptr_t)bootstrap;
        iargs[1] = selector;
        iargs[2] = cargc;
        iargs[3] = (int64_t)(uintptr_t)cargv;
        top = ((uintptr_t)j.stack + j.stack_len) & ~(uintptr_t)15;
        result = kp_call_guest(as_obj(__exc_send), (void *)top, iargs, fargs, &fret);

        kp_jit_free(&j);
        util_cleanup_run();
        return (int)result;
    }
}
