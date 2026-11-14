/* frontend.c : run the Kobold front end (kpcom) in-process to compile
   Kobold Pascal source to a KPM1 module (see frontend.h).

   Two builds, chosen by KP_FE_LINKED:

   - Linked (KP_FE_LINKED, the native host JIT): kpcom is linked into this binary
     as native code (compiled by skj-pc, the same object skj-kpc uses) beside the C
     module builder.  Compiling is one step: call kpcom's CompileModule export,
     which drives the builder, then serialize kpb_module_ptr() with kpm_write.  No
     seed, no JIT of the compiler, no KPM text hand-off through a guest string.

   - Seeded (the cross JITs, where the host-native kpcom object cannot link into a
     cross-arch binary): kpcom is itself a KPM1 module embedded by bin2c and JIT'd
     here.  It is single-step too: the JIT'd kpcom drives the host C module builder
     through the bound kpbp_* entry points (the guest passes runtime string
     descriptors, which the j_kpbp_* shims read with kp_str_bytes/kp_str_len and
     forward to the kpb_* builder core), and the finished module is serialized here
     with kpm_write, exactly as the linked path does.  No text module ever crosses
     the boundary; kpcom carries no text emitter. */

#define _POSIX_C_SOURCE 200809L   /* open_memstream (linked path) */

#include "frontend.h"
#include "kpm.h"
#include "kp_host.h"            /* kp_read_file */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#ifdef KP_FE_LINKED

/* kpcom's native entry points.  skj-pc lowercases the Pascal names, so the
   CompileModule export is `compilemodule`.  It takes two runtime string
   descriptors (source name and source text) and returns 1 on a clean compile, 0
   on a source error; the finished module is left in the C builder, reachable
   through kpb_module_ptr(). */
extern long compilemodule(const void *src_name, const void *src_text);

/* The host Pascal runtime's string constructor (pascal_rt_host): wrap a C buffer
   in a managed descriptor kpcom can read as a `const string` parameter. */
extern void *__pascal_str_new(const char *b, int len);

/* The C module builder's finished module (kpbuild.c). */
extern struct kpm_module *kpb_module_ptr(void);

/* kpcom's standalone ProcessFile path (its -o file output) references
   kpb_emit_native, but the JIT never runs ProcessFile: it calls CompileModule and
   serializes the builder's module here with kpm_write.  Provide a stub so the
   front-end object links without pulling in the AOT emit backend (kp_aot.c and
   target_emit), which this binary does not carry. */
int
kpb_emit_native(const void *outpath, int emit_kpm)
{
    (void)outpath;
    (void)emit_kpm;
    return 1;
}

/* kpcom's standalone compile path also lowers a prebuilt `.kpm` to assembly
   (kpb_emit_from_kpm), which the JIT likewise never runs.  Stub it for the same
   reason: the JIT binary carries no AOT emit backend. */
int
kpb_emit_from_kpm(const void *inpath, const void *outpath)
{
    (void)inpath;
    (void)outpath;
    return 1;
}

int
kp_compile_pas(const char *pas_path, char **module_text, size_t *out_len,
               char *err, size_t errlen)
{
    char *src;
    size_t srclen = 0;
    void *name_d;
    void *text_d;
    long ok;
    char *buf = NULL;
    size_t bufsz = 0;
    FILE *ms;

    err[0] = '\0';

    src = kp_read_file(pas_path, &srclen);
    if (!src) {
        snprintf(err, errlen, "cannot read '%s'", pas_path);
        return 1;
    }

    /* Hand the source to kpcom as runtime string descriptors.  kpcom compiles
       straight into the builder; the finished module stays in kpb_module_ptr(). */
    name_d = __pascal_str_new(pas_path, (int)strlen(pas_path));
    text_d = __pascal_str_new(src, (int)srclen);
    free(src);

    ok = compilemodule(name_d, text_d);
    if (!ok)
        return 2;                       /* kpcom already reported the source error */

    /* Serialize the builder's module once into an in-memory KPM1 image, the form
       the rest of the driver (add_module, kpm_read) already consumes. */
    ms = open_memstream(&buf, &bufsz);
    if (!ms) {
        snprintf(err, errlen, "out of memory");
        return 1;
    }
    if (kpm_write(ms, kpb_module_ptr()) != 0) {
        fclose(ms);
        free(buf);
        snprintf(err, errlen, "failed to serialize the compiled module");
        return 1;
    }
    fclose(ms);

    *module_text = buf;
    *out_len = bufsz;
    return 0;
}

#else /* seeded: the embedded KPM1 front end, JIT'd, driving the host builder */

#include "jit_common.h"
#include "kp_host.h"
#include "str.h"
#include "kpcom_kpm.h"
#include "kpbuild.h"

#include <dirent.h>

/* kpcom imports ReadFileStr (its command-line driver reads source files), so it
   must be bound even though the `compile` export takes its source as a string. */
static kp_gptr
fe_read_file_str(kp_gptr pathstr)
{
    char path[1024];
    int plen = kp_str_len(pathstr);
    char *buf;
    size_t n = 0;
    kp_gptr s;

    if (plen < 0 || plen >= (int)sizeof path)
        return kp_str_new("", 0);
    memcpy(path, kp_str_bytes(pathstr), (size_t)plen);
    path[plen] = '\0';
    buf = kp_read_file(path, &n);
    s = kp_str_new(buf ? buf : "", (int)n);
    free(buf);
    return s;
}

/* kpcom also imports WriteFileStr for its `-o` output path (reached only when the
   embedded seed is run as a standalone front end, not through the `compile`
   export), so it must be bound too or the seed fails to link under the JIT. */
static int
fe_write_file_str(kp_gptr pathstr, kp_gptr datastr)
{
    char path[1024];
    int plen = kp_str_len(pathstr);

    if (plen < 0 || plen >= (int)sizeof path)
        return 0;
    memcpy(path, kp_str_bytes(pathstr), (size_t)plen);
    path[plen] = '\0';
    return kp_write_file(path, kp_str_bytes(datastr), (size_t)kp_str_len(datastr));
}

/* Directory enumeration for the `uses ns.*` wildcard: kpcom lists a namespace directory at
   compile time. The same handle-based primitives the io.dirs unit uses, over the host
   opendir/readdir. A handle is an index into a small table of open DIR*; kpd_next returns one
   entry name at a time (the empty string at end). The jit_main and aot_rt host tiers carry
   their own copies for the standalone and native-AOT compilers. */
#define FE_KPD_MAX 16
static DIR *fe_kpd_tab[FE_KPD_MAX];

static int
fe_kpd_open(kp_gptr pathstr)
{
    char path[1024];
    int plen = kp_str_len(pathstr);
    int i;

    if (plen < 0 || plen >= (int)sizeof path)
        return -1;
    memcpy(path, kp_str_bytes(pathstr), (size_t)plen);
    path[plen] = '\0';
    for (i = 0; i < FE_KPD_MAX; i++)
        if (!fe_kpd_tab[i])
            break;
    if (i == FE_KPD_MAX)
        return -1;
    fe_kpd_tab[i] = opendir(path);
    return fe_kpd_tab[i] ? i : -1;
}

static kp_gptr
fe_kpd_next(int h)
{
    struct dirent *e;

    if (h < 0 || h >= FE_KPD_MAX || !fe_kpd_tab[h])
        return kp_str_new("", 0);
    e = readdir(fe_kpd_tab[h]);
    if (!e)
        return kp_str_new("", 0);
    return kp_str_new(e->d_name, (int)strlen(e->d_name));
}

static void
fe_kpd_close(int h)
{
    if (h >= 0 && h < FE_KPD_MAX && fe_kpd_tab[h]) {
        closedir(fe_kpd_tab[h]);
        fe_kpd_tab[h] = 0;
    }
}

static void *
fe_as_addr(void (*fn)(void))
{
    union { void (*f)(void); void *o; } u;
    u.f = fn;
    return u.o;
}

/* The module-builder bindings for the JIT'd kpcom.  The guest (single-step kpcom
   compiled with KP_ONESTEP) drives the C builder call by call through the kpbp_*
   imports; each call arrives here with its string arguments as guest runtime
   string descriptors.  Each shim materializes the descriptor's bytes into a C
   string with kp_str_bytes/kp_str_len (the same guest->host accessors the file-I/O
   bindings use) and forwards to the kpb_* builder core, which accumulates the
   module in host memory for kpm_write.  The linked path (KP_FE_LINKED) uses
   kpb_fe.c for the same job; here the descriptor pointer is a guest handle, not a
   host address, so these shims translate instead of reading the bytes directly. */
static char *
j_dup(kp_gptr d)
{
    int n = d ? kp_str_len(d) : 0;
    const char *b = d ? kp_str_bytes(d) : "";
    char *s;

    if (n < 0)
        n = 0;
    s = malloc((size_t)n + 1);
    memcpy(s, b, (size_t)n);
    s[n] = '\0';
    return s;
}

static void j_kpbp_begin(void) { kpb_begin(); }
static void j_kpbp_target(kp_gptr d) { char *s = j_dup(d); kpb_target(s); free(s); }
static void j_kpbp_kind(int kind, kp_gptr d) { char *s = j_dup(d); kpb_kind(kind, s); free(s); }
static void j_kpbp_entry(kp_gptr d) { char *s = j_dup(d); kpb_entry(s); free(s); }
static void j_kpbp_file(kp_gptr d) { char *s = j_dup(d); kpb_file(s); free(s); }
static void j_kpbp_dep(kp_gptr d, int64_t fp) { char *s = j_dup(d); kpb_dep(s, (unsigned int)fp); free(s); }
static void j_kpbp_export(kp_gptr d) { char *s = j_dup(d); kpb_export(s); free(s); }
static void j_kpbp_import_begin(int kind, kp_gptr d) { char *s = j_dup(d); kpb_import_begin(kind, s); free(s); }
static void j_kpbp_import_param(kp_gptr d) { char *s = j_dup(d); kpb_import_param(s); free(s); }
static void j_kpbp_import_ret(kp_gptr d) { char *s = j_dup(d); kpb_import_ret(s); free(s); }
static void j_kpbp_method_begin(kp_gptr d) { char *s = j_dup(d); kpb_method_begin(s); free(s); }
static void j_kpbp_method_param(kp_gptr d) { char *s = j_dup(d); kpb_method_param(s); free(s); }
static void j_kpbp_method_ret(kp_gptr d) { char *s = j_dup(d); kpb_method_ret(s); free(s); }

static void
j_kpbp_global_begin(kp_gptr name, kp_gptr base, int arr, int align, int flags)
{
    char *n = j_dup(name), *b = j_dup(base);
    kpb_global_begin(n, b, arr, align, flags);
    free(n);
    free(b);
}

static void j_kpbp_global_init_int(int64_t v) { kpb_global_init_int(v); }
static void j_kpbp_global_init_sym(kp_gptr d) { char *s = j_dup(d); kpb_global_init_sym(s); free(s); }

static void
j_kpbp_global_string(kp_gptr d)
{
    /* the literal's bytes may contain NULs: pass bytes + length, no NUL-terminate */
    int n = d ? kp_str_len(d) : 0;
    if (n < 0)
        n = 0;
    kpb_global_string(d ? kp_str_bytes(d) : "", n);
}

static void j_kpbp_global_end(void) { kpb_global_end(); }
static void j_kpbp_iface_begin(int64_t fp) { kpb_iface_begin((unsigned int)fp); }
static void j_kpbp_iface_line(kp_gptr d) { char *s = j_dup(d); kpb_iface_line(s); free(s); }

static void
j_kpbp_func_begin(kp_gptr name, int is_local, int is_variadic,
                  int nparams, int nslots, int ntemps, int nlabels)
{
    char *s = j_dup(name);
    kpb_func_begin(s, is_local, is_variadic, nparams, nslots, ntemps, nlabels);
    free(s);
}

static void j_kpbp_slotsize(int size) { kpb_slotsize(size); }
static void j_kpbp_paramclass(int cls) { kpb_paramclass(cls); }
static void j_kpbp_paramfw(int fw) { kpb_paramfw(fw); }

static void
j_kpbp_insn(kp_gptr mnem, int dst, int a, int b, int64_t imm,
            int slot, int lab, int nargs, kp_gptr sym)
{
    char *m = j_dup(mnem), *y = j_dup(sym);
    kpb_insn(m, dst, a, b, imm, slot, lab, nargs, y);
    free(m);
    free(y);
}

static int
j_kpbp_asm(kp_gptr d)
{
    char *s = j_dup(d);
    int r = kpb_asm_text(s);
    free(s);
    return r;
}

static void j_kpbp_func_end(void) { kpb_func_end(); }

/* kpcom's standalone ProcessFile path imports kpb_emit_native for its -o output,
   but the JIT drives compilemodule and serializes kpb_module_ptr() here, never
   ProcessFile.  The guest module still declares the import, so bind a stub. */
static int j_kpb_emit_native(kp_gptr p, int e) { (void)p; (void)e; return 1; }

/* Likewise kpcom's .kpm -> native path (ProcessFile when the input is a prebuilt
   module) imports kpb_emit_from_kpm; the JIT never takes that path either, so
   bind a stub to satisfy the guest's declared import. */
static int j_kpb_emit_from_kpm(kp_gptr in, kp_gptr out) { (void)in; (void)out; return 1; }

/* Bind every kpbp_* the guest imports into the builder shims above. Returns 0, or
   -1 (with err set) if the binding table is full. */
static int
fe_bind_builder(struct kp_binding *binds, int *nbinds, int cap)
{
    static const struct { const char *name; void (*fn)(void); } tab[] = {
        { "kpbp_begin",        (void (*)(void))j_kpbp_begin },
        { "kpbp_target",       (void (*)(void))j_kpbp_target },
        { "kpbp_kind",         (void (*)(void))j_kpbp_kind },
        { "kpbp_entry",        (void (*)(void))j_kpbp_entry },
        { "kpbp_file",         (void (*)(void))j_kpbp_file },
        { "kpbp_dep",          (void (*)(void))j_kpbp_dep },
        { "kpbp_export",       (void (*)(void))j_kpbp_export },
        { "kpbp_import_begin", (void (*)(void))j_kpbp_import_begin },
        { "kpbp_import_param", (void (*)(void))j_kpbp_import_param },
        { "kpbp_import_ret",   (void (*)(void))j_kpbp_import_ret },
        { "kpbp_method_begin", (void (*)(void))j_kpbp_method_begin },
        { "kpbp_method_param", (void (*)(void))j_kpbp_method_param },
        { "kpbp_method_ret",   (void (*)(void))j_kpbp_method_ret },
        { "kpbp_global_begin", (void (*)(void))j_kpbp_global_begin },
        { "kpbp_global_init_int", (void (*)(void))j_kpbp_global_init_int },
        { "kpbp_global_init_sym", (void (*)(void))j_kpbp_global_init_sym },
        { "kpbp_global_string", (void (*)(void))j_kpbp_global_string },
        { "kpbp_global_end",   (void (*)(void))j_kpbp_global_end },
        { "kpbp_iface_begin",  (void (*)(void))j_kpbp_iface_begin },
        { "kpbp_iface_line",   (void (*)(void))j_kpbp_iface_line },
        { "kpbp_func_begin",   (void (*)(void))j_kpbp_func_begin },
        { "kpbp_slotsize",     (void (*)(void))j_kpbp_slotsize },
        { "kpbp_paramclass",   (void (*)(void))j_kpbp_paramclass },
        { "kpbp_paramfw",      (void (*)(void))j_kpbp_paramfw },
        { "kpbp_insn",         (void (*)(void))j_kpbp_insn },
        { "kpbp_asm",          (void (*)(void))j_kpbp_asm },
        { "kpbp_func_end",     (void (*)(void))j_kpbp_func_end },
        { "kpb_emit_native",   (void (*)(void))j_kpb_emit_native },
        { "kpb_emit_from_kpm", (void (*)(void))j_kpb_emit_from_kpm },
    };
    int i;
    int n = (int)(sizeof tab / sizeof tab[0]);

    if (*nbinds + n > cap)
        return -1;
    for (i = 0; i < n; i++) {
        binds[*nbinds].name = tab[i].name;
        binds[*nbinds].addr = fe_as_addr(tab[i].fn);
        (*nbinds)++;
    }
    return 0;
}

int
kp_compile_pas(const char *pas_path, char **module_text, size_t *out_len,
               char *err, size_t errlen)
{
    struct kp_binding binds[256];
    int nbinds = 0;
    const unsigned char *mod_bytes = kpcom_kpm;
    size_t mod_len = kpcom_kpm_len;
    char *mod_owned = NULL;          /* set when KPCOM_KPM overrides the seed */
    const char *override;
    struct kpm_module m;
    struct kp_jit j;
    char *src = NULL;
    size_t srclen = 0;
    kp_gptr name_s, text_s;
    void *entry;
    int64_t iargs[6] = { 0 };
    double fargs[8] = { 0 };
    double fret = 0;
    uint64_t result;
    uintptr_t top;
    int rc = 1;
    int have_jit = 0;
    int mod_parsed = 0;

    err[0] = '\0';

    /* The front end: the embedded seed, or a file named by KPCOM_KPM. */
    override = getenv("KPCOM_KPM");
    if (override && override[0]) {
        size_t n = 0;
        mod_owned = kp_read_file(override, &n);
        if (!mod_owned) {
            snprintf(err, errlen, "cannot read KPCOM_KPM '%s'", override);
            return 1;
        }
        mod_bytes = (const unsigned char *)mod_owned;
        mod_len = n;
    }

    /* The user's source: the `compile` export takes its text, not a path. */
    src = kp_read_file(pas_path, &srclen);
    if (!src) {
        snprintf(err, errlen, "cannot read '%s'", pas_path);
        free(mod_owned);
        return 1;
    }

    /* Bind the standard runtime, the file I/O kpcom imports (prefixed kpc_ so they
       stay distinct from the Kobold runtime's own ReadFileStr when both live in one
       binary: source reads, the namespace-wildcard directory listing), and the
       module builder the single-step guest drives. */
    kp_install_runtime(binds, &nbinds, (int)(sizeof binds / sizeof binds[0]));
    if (nbinds + 5 <= (int)(sizeof binds / sizeof binds[0])) {
        binds[nbinds].name = "kpc_ReadFileStr";
        binds[nbinds].addr = fe_as_addr((void (*)(void))fe_read_file_str);
        nbinds++;
        binds[nbinds].name = "kpc_WriteFileStr";
        binds[nbinds].addr = fe_as_addr((void (*)(void))fe_write_file_str);
        nbinds++;
        /* the unit-namespace wildcard lists a namespace directory at compile time */
        binds[nbinds].name = "kpc_kpd_open";
        binds[nbinds].addr = fe_as_addr((void (*)(void))fe_kpd_open);
        nbinds++;
        binds[nbinds].name = "kpc_kpd_next";
        binds[nbinds].addr = fe_as_addr((void (*)(void))fe_kpd_next);
        nbinds++;
        binds[nbinds].name = "kpc_kpd_close";
        binds[nbinds].addr = fe_as_addr((void (*)(void))fe_kpd_close);
        nbinds++;
    }
    if (fe_bind_builder(binds, &nbinds, (int)(sizeof binds / sizeof binds[0])) != 0) {
        snprintf(err, errlen, "binding table too small for the module builder");
        goto done;
    }

    if (kpm_read(&m, (char *)mod_bytes, mod_len, err, errlen) != 0)
        goto done;
    mod_parsed = 1;
    if (kp_jit(&j, &m.prog, binds, nbinds, err, errlen) != 0)
        goto done;
    have_jit = 1;

    entry = kp_jit_entry(&j, "CompileModule");
    if (!entry) {
        snprintf(err, errlen, "the front end has no 'CompileModule' export");
        goto done;
    }

    /* compilemodule(srcName, srcText): two guest strings in, 1/0 out. The guest
       lowers straight into the host builder through the bound kpbp_*; the finished
       module is left in kpb_module_ptr() here, as in the linked path. The front
       end's pointer width is baked into its binary (native per target), so the
       export sets its own target and takes no width argument. */
    name_s = kp_str_new(pas_path, (int)strlen(pas_path));
    text_s = kp_str_new(src, (int)srclen);
    iargs[0] = name_s;
    iargs[1] = text_s;

    top = ((uintptr_t)j.stack + j.stack_len) & ~(uintptr_t)15;
    result = kp_call_guest(entry, (void *)top, iargs, fargs, &fret);

    /* CompileModule returns a 32-bit LongInt (1 clean, 0 on a compile error).
       kp_call_guest hands back the full integer-return register pair, whose high
       word is undefined for a 32-bit result: on rv32 a1 is left as the guest set
       it, and on arm64 the top half of x0 is unspecified per the psABI (only
       x86-64 zero-extends eax). Read just the low 32 bits, the width the callee
       actually returned, so a 0 return is not misread as success. */
    if ((int32_t)result == 0) {
        /* a compile error; kpcom already wrote its diagnostics to stderr */
        rc = 2;
        goto done;
    }
    {
        /* Serialize the builder's module into an in-memory KPM1 image, the form the
           rest of the driver (add_module, kpm_read) consumes. */
        char *buf = NULL;
        size_t bufsz = 0;
        FILE *ms = open_memstream(&buf, &bufsz);

        if (!ms) {
            snprintf(err, errlen, "out of memory");
            goto done;
        }
        if (kpm_write(ms, kpb_module_ptr()) != 0) {
            fclose(ms);
            free(buf);
            snprintf(err, errlen, "failed to serialize the compiled module");
            goto done;
        }
        fclose(ms);
        *module_text = buf;
        *out_len = bufsz;
        rc = 0;
    }

done:
    if (have_jit)
        kp_jit_free(&j);
    if (mod_parsed)
        kpm_free(&m);
    free(src);
    free(mod_owned);
    return rc;
}

#endif /* KP_FE_LINKED */
