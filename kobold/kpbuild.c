/* kpbuild.c : the in-memory KPM module builder.

   The single-step Kobold compiler links this beside the front end.  The front
   end drives these entry points as it lowers, handing structured IR and module
   metadata across the boundary instead of writing KPM text, and the driver then
   takes the finished kpm_module (kpb_module_ptr) and either emits assembly
   through kp_aot_emit or serializes it with kpm_write for --emit-kpm.

   The entry points are called by name from the front end (external cdecl), so
   they keep a module in a file-static instance rather than threading a handle.
   Top-level lists (imports, exports, deps, interface lines) accumulate in
   malloc scratch and are copied into the module arena when the module is taken;
   functions and globals are built as arena-owned linked lists directly, and an
   import's parameter classes and a type's methods finalize at their close. */

#include "kpbuild.h"
#include "ir.h"

#include <stdlib.h>
#include <string.h>

/* A growable vector of char* (arena-owned strings or class tokens). */
struct svec {
    char **v;
    int n, cap;
};

static void
svec_push(struct svec *s, char *p)
{
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 8;
        s->v = realloc(s->v, (size_t)s->cap * sizeof(char *));
    }
    s->v[s->n++] = p;
}

static void
svec_clear(struct svec *s)
{
    free(s->v);
    s->v = NULL;
    s->n = s->cap = 0;
}

/* A growable vector of int64. */
struct ivec {
    int64_t *v;
    int n, cap;
};

static void
ivec_push(struct ivec *s, int64_t x)
{
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 8;
        s->v = realloc(s->v, (size_t)s->cap * sizeof(int64_t));
    }
    s->v[s->n++] = x;
}

static void
ivec_clear(struct ivec *s)
{
    free(s->v);
    s->v = NULL;
    s->n = s->cap = 0;
}

/****************************************************************
 * Builder state
 ****************************************************************/

static struct kpm_module M;
static int initialized;

/* top-level lists */
static struct svec exports;         /* char* export names */
static struct { struct kp_import *v; int n, cap; } imports;
static struct { struct kp_dep *v; int n, cap; } deps;
static struct svec iface;           /* interface body lines */

/* current import being built */
static struct svec imp_params;      /* class tokens for the current func/helper import */
static struct { struct kp_method *v; int n, cap; } imp_methods;  /* for a type import */
static struct svec meth_params;     /* class tokens for the current method */
static char *meth_name;
static int building_type;

/* function and global linked-list tails (arena-owned) */
static struct ir_func *ftail;
static struct ir_global *gtail;
static struct ir_func *cur_fn;
static struct ir_global *cur_g;
static int slot_i, pclass_i, pfw_i;

/* current global initializer scratch */
static struct ivec g_ivals;
static struct svec g_syms;          /* parallel: a name for a sym init, else NULL */

/* current extended-asm scratch */
static struct ir_asm_info *cur_asm;
static int asm_op_i, asm_clob_i;

static char *
adup(const char *s)
{
    return s ? arena_strdup(&M.arena, s) : NULL;
}

void
kpb_begin(void)
{
    if (initialized)
        arena_free(&M.arena);
    memset(&M, 0, sizeof M);
    arena_init(&M.arena);
    initialized = 1;

    svec_clear(&exports);
    free(imports.v); imports.v = NULL; imports.n = imports.cap = 0;
    free(deps.v); deps.v = NULL; deps.n = deps.cap = 0;
    svec_clear(&iface);
    svec_clear(&imp_params);
    free(imp_methods.v); imp_methods.v = NULL; imp_methods.n = imp_methods.cap = 0;
    svec_clear(&meth_params);
    meth_name = NULL;
    building_type = 0;
    ftail = NULL;
    gtail = NULL;
    cur_fn = NULL;
    cur_g = NULL;
    cur_asm = NULL;
    ivec_clear(&g_ivals);
    svec_clear(&g_syms);
    M.iface_fp = 0;
}

/****************************************************************
 * Header and metadata
 ****************************************************************/

void
kpb_target(const char *tag)
{
    M.target = adup(tag);
}

void
kpb_kind(int kind, const char *name)
{
    M.kind = kind;
    if (name && name[0])
        M.name = adup(name);
}

void
kpb_entry(const char *name)
{
    M.entry = adup(name);
}

void
kpb_file(const char *src)
{
    M.source_file = adup(src);
}

void
kpb_dep(const char *name, unsigned int fp)
{
    if (deps.n == deps.cap) {
        deps.cap = deps.cap ? deps.cap * 2 : 8;
        deps.v = realloc(deps.v, (size_t)deps.cap * sizeof(struct kp_dep));
    }
    deps.v[deps.n].name = adup(name);
    deps.v[deps.n].fingerprint = fp;
    deps.n++;
}

void
kpb_export(const char *name)
{
    svec_push(&exports, adup(name));
}

/****************************************************************
 * Imports
 ****************************************************************/

/* Finalize the class tokens the current func/helper import or method collected
   into an arena array, returning it and its count. */
static char **
finish_classes(struct svec *s, int *np)
{
    char **out = NULL;
    *np = s->n;
    if (s->n) {
        out = arena_alloc(&M.arena, (size_t)s->n * sizeof(char *));
        for (int i = 0; i < s->n; i++)
            out[i] = s->v[i];
    }
    s->n = 0;   /* reuse the scratch; strings already live in the arena */
    return out;
}

/* Close any type import that was open, copying its collected methods into the
   module arena. */
static void
close_type_import(void)
{
    if (!building_type)
        return;
    struct kp_import *imp = &imports.v[imports.n - 1];
    imp->nmethods = imp_methods.n;
    if (imp_methods.n) {
        imp->methods = arena_alloc(&M.arena,
                                   (size_t)imp_methods.n * sizeof(struct kp_method));
        memcpy(imp->methods, imp_methods.v,
               (size_t)imp_methods.n * sizeof(struct kp_method));
    }
    imp_methods.n = 0;
    building_type = 0;
}

void
kpb_import_begin(int kind, const char *name)
{
    close_type_import();
    if (imports.n == imports.cap) {
        imports.cap = imports.cap ? imports.cap * 2 : 8;
        imports.v = realloc(imports.v,
                            (size_t)imports.cap * sizeof(struct kp_import));
    }
    struct kp_import *imp = &imports.v[imports.n++];
    memset(imp, 0, sizeof *imp);
    imp->kind = kind;
    imp->name = adup(name);
    imp_params.n = 0;
    if (kind == KP_IMPORT_TYPE)
        building_type = 1;
}

void
kpb_import_param(const char *cls)
{
    svec_push(&imp_params, adup(cls));
}

void
kpb_import_ret(const char *cls)
{
    struct kp_import *imp = &imports.v[imports.n - 1];
    imp->param_cls = finish_classes(&imp_params, &imp->nparams);
    imp->ret_cls = adup(cls);
}

void
kpb_method_begin(const char *name)
{
    meth_name = adup(name);
    meth_params.n = 0;
}

void
kpb_method_param(const char *cls)
{
    svec_push(&meth_params, adup(cls));
}

void
kpb_method_ret(const char *cls)
{
    if (imp_methods.n == imp_methods.cap) {
        imp_methods.cap = imp_methods.cap ? imp_methods.cap * 2 : 8;
        imp_methods.v = realloc(imp_methods.v,
                                (size_t)imp_methods.cap * sizeof(struct kp_method));
    }
    struct kp_method *me = &imp_methods.v[imp_methods.n++];
    me->name = meth_name;
    me->param_cls = finish_classes(&meth_params, &me->nparams);
    me->ret_cls = adup(cls);
}

/****************************************************************
 * Globals
 ****************************************************************/

void
kpb_global_begin(const char *name, const char *base, int arr, int align, int flags)
{
    struct ir_global *g = arena_zalloc(&M.arena, sizeof *g);
    g->name = adup(name);
    g->base_type = kpm_base_type_by_name(base);
    if (g->base_type < 0)
        g->base_type = IR_I32;
    g->arr_size = arr;
    g->align = align;
    g->is_export = (flags & KPB_G_EXPORT) != 0;
    g->is_local = (flags & KPB_G_EXPORT) == 0;
    g->is_ptr = (flags & KPB_G_PTR) != 0;
    g->is_rodata = (flags & KPB_G_RODATA) != 0;
    g->is_strobj = (flags & KPB_G_STROBJ) != 0;
    if (gtail)
        gtail->next = g;
    else
        M.prog.globals = g;
    gtail = g;
    cur_g = g;
    g_ivals.n = 0;
    g_syms.n = 0;
}

void
kpb_global_init_int(long long v)
{
    ivec_push(&g_ivals, (int64_t)v);
    svec_push(&g_syms, NULL);
}

void
kpb_global_init_sym(const char *name)
{
    ivec_push(&g_ivals, 0);
    svec_push(&g_syms, adup(name));
}

void
kpb_global_string(const char *bytes, int len)
{
    char *buf = arena_alloc(&M.arena, (size_t)len + 1);
    memcpy(buf, bytes, (size_t)len);
    buf[len] = '\0';
    cur_g->init_string = buf;
    cur_g->init_strlen = len;
}

void
kpb_global_end(void)
{
    int n = g_ivals.n;
    if (n) {
        cur_g->init_ivals = arena_alloc(&M.arena, (size_t)n * sizeof(int64_t));
        cur_g->init_syms = arena_alloc(&M.arena, (size_t)n * sizeof(char *));
        for (int i = 0; i < n; i++) {
            cur_g->init_ivals[i] = g_ivals.v[i];
            cur_g->init_syms[i] = g_syms.v[i];
        }
        cur_g->init_count = n;
    }
    cur_g = NULL;
}

/****************************************************************
 * Interface block
 ****************************************************************/

void
kpb_iface_begin(unsigned int fp)
{
    M.iface_fp = fp;
}

void
kpb_iface_line(const char *line)
{
    svec_push(&iface, adup(line));
}

/****************************************************************
 * Functions and instructions
 ****************************************************************/

void
kpb_func_begin(const char *name, int is_local, int is_variadic,
               int nparams, int nslots, int ntemps, int nlabels)
{
    struct ir_func *fn = ir_new_func(&M.arena, name);
    fn->is_local = is_local;
    fn->is_variadic = is_variadic;
    fn->nparams = nparams;
    fn->nslots = nslots;
    fn->ntemps = ntemps;
    fn->nlabels = nlabels;
    if (nslots)
        fn->slot_size = arena_zalloc(&M.arena, (size_t)nslots * sizeof(int));
    if (nparams) {
        fn->param_class = arena_zalloc(&M.arena, (size_t)nparams * sizeof(int));
        fn->param_fw = arena_zalloc(&M.arena, (size_t)nparams * sizeof(signed char));
    }
    if (ftail)
        ftail->next = fn;
    else
        M.prog.funcs = fn;
    ftail = fn;
    cur_fn = fn;
    slot_i = pclass_i = pfw_i = 0;
}

void
kpb_slotsize(int size)
{
    if (cur_fn->slot_size && slot_i < cur_fn->nslots)
        cur_fn->slot_size[slot_i++] = size;
}

void
kpb_paramclass(int cls)
{
    if (cur_fn->param_class && pclass_i < cur_fn->nparams)
        cur_fn->param_class[pclass_i++] = cls;
}

void
kpb_paramfw(int fw)
{
    if (cur_fn->param_fw && pfw_i < cur_fn->nparams)
        cur_fn->param_fw[pfw_i++] = (signed char)fw;
}

void
kpb_insn(const char *mnem, int dst, int a, int b, long long imm,
         int slot, int label, int nargs, const char *sym)
{
    int op = kpm_op_by_name(mnem);
    struct ir_insn *in;

    if (op < 0)
        return;
    in = ir_emit(cur_fn, op);
    in->dst = dst;
    in->a = a;
    in->b = b;
    in->imm = imm;
    in->slot = slot;
    in->label = label;
    in->nargs = nargs;
    in->sym = adup(sym);
}

void
kpb_asm_basic(const char *tmpl)
{
    struct ir_insn *in = ir_emit(cur_fn, IR_ASM);
    in->sym = adup(tmpl);
}

void
kpb_asm_ext_begin(int nout, int nin, int vol)
{
    int nops = nout + nin;
    cur_asm = arena_zalloc(&M.arena, sizeof *cur_asm);
    cur_asm->noutputs = nout;
    cur_asm->ninputs = nin;
    cur_asm->is_volatile = vol;
    if (nops)
        cur_asm->ops = arena_zalloc(&M.arena, (size_t)nops * sizeof(struct ir_asm_op));
    asm_op_i = asm_clob_i = 0;
}

void
kpb_asm_op(int role, const char *constraint, int temp, int size)
{
    int nops = cur_asm->noutputs + cur_asm->ninputs;
    if (asm_op_i >= nops)
        return;
    struct ir_asm_op *op = &cur_asm->ops[asm_op_i++];
    op->constraint = adup(constraint);
    op->pin_reg = NULL;
    op->temp = temp;
    op->size = size;
    op->is_output = (role == KPB_ASM_OUT || role == KPB_ASM_INOUT);
    op->is_inout = (role == KPB_ASM_INOUT);
}

void
kpb_asm_clobber(const char *name)
{
    if (asm_clob_i == cur_asm->nclobbers) {
        int nc = cur_asm->nclobbers + 1;
        char **grown = arena_alloc(&M.arena, (size_t)nc * sizeof(char *));
        for (int i = 0; i < cur_asm->nclobbers; i++)
            grown[i] = cur_asm->clobbers[i];
        cur_asm->clobbers = grown;
        cur_asm->nclobbers = nc;
    }
    cur_asm->clobbers[asm_clob_i++] = adup(name);
}

void
kpb_asm_ext_end(const char *tmpl)
{
    struct ir_insn *in = ir_emit(cur_fn, IR_ASM);
    in->sym = adup(tmpl);
    in->asm_info = cur_asm;
    cur_asm = NULL;
}

int
kpb_asm_text(const char *args)
{
    /* The front end carries an ASM instruction as the preformatted operand text
       (the part after "ASM"): a quoted template for basic asm, or the operand and
       clobber metadata then the quoted template for extended asm.  Rebuild the
       "ASM <args>" line and parse it with the shared asm-line parser, so the one
       grammar has one parser.  Returns 0, or -1 on a malformed line. */
    size_t n = strlen(args);
    char *line = arena_alloc(&M.arena, n + 5);
    const char *msg = NULL;
    int r;

    memcpy(line, "ASM ", 4);
    memcpy(line + 4, args, n + 1);
    r = kpm_emit_asm_line(&M.arena, cur_fn, line, &msg);
    return r < 0 ? -1 : 0;
}

void
kpb_func_end(void)
{
    struct ir_func *fn = cur_fn;

    /* Mirror kpm_read's read_func: derive the psABI param_cls/param_neb from
       the scalar param_class the front end supplies (one eightbyte each, SSE
       for a float, INTEGER otherwise), so the shared selectors read the right
       register for a float parameter. */
    if (fn->nparams && fn->param_class) {
        fn->param_neb = arena_zalloc(&M.arena,
                                     (size_t)fn->nparams * sizeof(signed char));
        fn->param_cls = arena_zalloc(&M.arena,
                                     (size_t)fn->nparams * 4 * sizeof(signed char));
        for (int pi = 0; pi < fn->nparams; pi++) {
            fn->param_neb[pi] = 1;
            fn->param_cls[4 * pi] = (signed char)(fn->param_class[pi] == 1 ? 1 : 0);
        }
    }
    cur_fn = NULL;
}

/****************************************************************
 * Finalize
 ****************************************************************/

struct kpm_module *
kpb_module_ptr(void)
{
    close_type_import();

    M.nexports = exports.n;
    if (exports.n) {
        M.exports = arena_alloc(&M.arena, (size_t)exports.n * sizeof(char *));
        for (int i = 0; i < exports.n; i++)
            M.exports[i] = exports.v[i];
    }
    M.nimports = imports.n;
    if (imports.n) {
        M.imports = arena_alloc(&M.arena,
                                (size_t)imports.n * sizeof(struct kp_import));
        memcpy(M.imports, imports.v,
               (size_t)imports.n * sizeof(struct kp_import));
    }
    M.ndeps = deps.n;
    if (deps.n) {
        M.deps = arena_alloc(&M.arena, (size_t)deps.n * sizeof(struct kp_dep));
        memcpy(M.deps, deps.v, (size_t)deps.n * sizeof(struct kp_dep));
    }
    M.niface_lines = iface.n;
    if (iface.n) {
        M.iface_lines = arena_alloc(&M.arena, (size_t)iface.n * sizeof(char *));
        for (int i = 0; i < iface.n; i++)
            M.iface_lines[i] = iface.v[i];
    }

    /* Carry the .file hint onto each function, as kpm_read does. */
    for (struct ir_func *fn = M.prog.funcs; fn; fn = fn->next)
        fn->src_file = M.source_file;

    return &M;
}
