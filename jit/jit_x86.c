/* jit_x86.c : JIT an ir_program to executable x86-64 code, in-process.

   The instruction selector maps the shared IR to machine code through the
   jit/emit_x86.c byte encoder, the counterpart of backend/x86_emit.c on the
   AOT side.  It reuses backend/regalloc_x86.c (the same allocator the AOT
   path uses) and lays globals and code into low-2GB executable mappings the
   host calls in-process. */

#define _POSIX_C_SOURCE 200809L

#include "jit_x86.h"
#include "emit_x86.h"
#include "mc.h"
#include "x86_select.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>


/* A deferred intra-module call: patch the rel32 at `site` to the target. */
struct callreloc {
    size_t site;
    const char *target;
};

struct emitctx {
    struct kp_jit *j;
    struct ir_program *prog;
    const struct kp_binding *binds;
    int nbinds;
    struct callreloc *relocs;       /* rel32 intra-module calls */
    int nrelocs, crelocs;
    struct callreloc *absrelocs;    /* imm64 function code pointers (LEA) */
    int nabs, cabs;
    struct kp_line *lines;          /* address-to-line entries from IR_LOC (-g) */
    int nlines, clines;
    char *err;
    size_t errlen;
};

static int
fail(struct emitctx *e, const char *fmt, const char *a, const char *b)
{
    if (e->err && e->errlen)
        snprintf(e->err, e->errlen, fmt, a, b);
    return -1;
}

/****************************************************************
 * Layout helpers
 ****************************************************************/

static int
base_size(int bt)
{
    switch (bt) {
    case IR_I8: return 1;
    case IR_I16: return 2;
    case IR_I32: case IR_F32: return 4;
    case IR_I64: case IR_F64: return 8;
    default: return 4;
    }
}


static uint8_t *
global_addr(struct kp_jit *j, const char *name)
{
    for (int i = 0; i < j->nglobals; i++)
        if (strcmp(j->globals[i].name, name) == 0)
            return j->globals[i].addr;
    return NULL;
}

static void *
import_addr(struct emitctx *e, const char *name)
{
    for (int i = 0; i < e->nbinds; i++)
        if (strcmp(e->binds[i].name, name) == 0)
            return e->binds[i].addr;
    return NULL;
}

/* Resolve a data-relocation target to its runtime address: a global's data
   home, a function's code entry, or a host binding.  Returns 1 and sets *out
   on success.  Called after data and code are laid out. */
static int
resolve_data_sym(struct kp_jit *j, const struct kp_binding *binds, int nbinds,
                 const char *name, uint64_t *out)
{
    for (int k = 0; k < j->nglobals; k++)
        if (strcmp(j->globals[k].name, name) == 0) {
            *out = (uint64_t)(uintptr_t)j->globals[k].addr;
            return 1;
        }
    for (int k = 0; k < j->nfuncs; k++)
        if (strcmp(j->funcs[k].name, name) == 0) {
            *out = (uint64_t)(uintptr_t)(j->code + j->funcs[k].off);
            return 1;
        }
    for (int k = 0; k < nbinds; k++)
        if (strcmp(binds[k].name, name) == 0) {
            *out = (uint64_t)(uintptr_t)binds[k].addr;
            return 1;
        }
    return 0;
}

static int
is_module_func(struct ir_program *prog, const char *name)
{
    for (struct ir_func *fn = prog->funcs; fn; fn = fn->next)
        if (strcmp(fn->name, name) == 0)
            return 1;
    return 0;
}


static void
add_reloc(struct emitctx *e, size_t site, const char *target)
{
    if (e->nrelocs == e->crelocs) {
        e->crelocs = e->crelocs ? e->crelocs * 2 : 32;
        e->relocs = realloc(e->relocs, (size_t)e->crelocs * sizeof(*e->relocs));
    }
    e->relocs[e->nrelocs].site = site;
    e->relocs[e->nrelocs].target = target;
    e->nrelocs++;
}

/* Record an absolute (imm64) reference to a function's code address, patched
   into the mapped code once its final address is known. */
static void
add_absreloc(struct emitctx *e, size_t site, const char *target)
{
    if (e->nabs == e->cabs) {
        e->cabs = e->cabs ? e->cabs * 2 : 16;
        e->absrelocs = realloc(e->absrelocs, (size_t)e->cabs * sizeof(*e->absrelocs));
    }
    e->absrelocs[e->nabs].site = site;
    e->absrelocs[e->nabs].target = target;
    e->nabs++;
}

/* Record that code emitted from offset `off` onward comes from source `line`.
   Called for each IR_LOC marker; offsets grow monotonically, so the table stays
   sorted and a trap maps its address to the last entry at or before it. */
static void
add_line(struct emitctx *e, size_t off, int line)
{
    if (e->nlines == e->clines) {
        e->clines = e->clines ? e->clines * 2 : 64;
        e->lines = realloc(e->lines, (size_t)e->clines * sizeof(*e->lines));
    }
    e->lines[e->nlines].off = off;
    e->lines[e->nlines].line = line;
    e->nlines++;
}

/****************************************************************
 * Byte sink: the mc.h machine-code sink over the emit_x86.c encoder.
 *
 * The shared x86-64 selector (backend/x86_select.c, in a later slice) emits
 * through mc_*; this is the implementation that writes machine-code bytes for
 * the in-process JIT.  A struct mc bundles the growable code buffer, the emit
 * context (globals, host bindings, relocations, the line table), and a per-
 * function label table with forward-reference fixups.  The text sink
 * (backend/mc_text.c) is the AOT counterpart.
 ****************************************************************/

struct mclabel {
    long off;               /* byte offset where bound, or -1 if unbound */
    size_t *fix;            /* pending rel32 sites to patch when it binds */
    int nfix, cfix;
};

struct mc {
    struct code *c;
    struct emitctx *e;      /* globals, bindings, relocations, lines, prog */
    struct mclabel *lab;
    int nlab, clab;
    const char *errsym;     /* an unresolved symbol name, or NULL */
};

static void
mc_reset(struct mc *m, struct code *c, struct emitctx *e)
{
    m->c = c;
    m->e = e;
    m->lab = NULL;
    m->nlab = m->clab = 0;
    m->errsym = NULL;
}

static void
mc_free(struct mc *m)
{
    for (int i = 0; i < m->nlab; i++)
        free(m->lab[i].fix);
    free(m->lab);
    m->lab = NULL;
    m->nlab = m->clab = 0;
}

int
mc_new_label(struct mc *m)
{
    if (m->nlab == m->clab) {
        m->clab = m->clab ? m->clab * 2 : 32;
        m->lab = realloc(m->lab, (size_t)m->clab * sizeof(*m->lab));
    }
    m->lab[m->nlab].off = -1;
    m->lab[m->nlab].fix = NULL;
    m->lab[m->nlab].nfix = m->lab[m->nlab].cfix = 0;
    return m->nlab++;
}

void
mc_label(struct mc *m, int id)
{
    struct mclabel *l = &m->lab[id];
    l->off = (long)m->c->len;
    for (int i = 0; i < l->nfix; i++)
        x_patch_rel32(m->c, l->fix[i], (size_t)l->off);
    free(l->fix);
    l->fix = NULL;
    l->nfix = l->cfix = 0;
}

/* Record a rel32 site targeting label `id`: patch it now if bound, else defer. */
static void
mc_ref(struct mc *m, int id, size_t site)
{
    struct mclabel *l = &m->lab[id];
    if (l->off >= 0) {
        x_patch_rel32(m->c, site, (size_t)l->off);
        return;
    }
    if (l->nfix == l->cfix) {
        l->cfix = l->cfix ? l->cfix * 2 : 4;
        l->fix = realloc(l->fix, (size_t)l->cfix * sizeof(*l->fix));
    }
    l->fix[l->nfix++] = site;
}

void mc_jmp(struct mc *m, int id) { mc_ref(m, id, x_jmp_rel32(m->c)); }
void mc_jcc(struct mc *m, int cc, int id) { mc_ref(m, id, x_jcc_rel32(m->c, cc)); }

/* Data movement. */
void mc_mov_ri32(struct mc *m, int r, uint32_t v) { x_mov_ri32(m->c, r, v); }
void mc_mov_ri64(struct mc *m, int r, uint64_t v) { x_mov_ri64(m->c, r, v); }
void mc_mov_rr(struct mc *m, int d, int s) { x_mov_rr(m->c, d, s); }
void mc_mov32_rr(struct mc *m, int d, int s) { x_mov32_rr(m->c, d, s); }

/* 32-bit integer arithmetic. */
void mc_add32_rr(struct mc *m, int d, int s) { x_add32_rr(m->c, d, s); }
void mc_sub32_rr(struct mc *m, int d, int s) { x_sub32_rr(m->c, d, s); }
void mc_and32_rr(struct mc *m, int d, int s) { x_and32_rr(m->c, d, s); }
void mc_or32_rr(struct mc *m, int d, int s) { x_or32_rr(m->c, d, s); }
void mc_xor32_rr(struct mc *m, int d, int s) { x_xor32_rr(m->c, d, s); }
void mc_imul32_rr(struct mc *m, int d, int s) { x_imul32_rr(m->c, d, s); }
void mc_neg32_r(struct mc *m, int r) { x_neg32_r(m->c, r); }
void mc_not32_r(struct mc *m, int r) { x_not32_r(m->c, r); }

/* 64-bit integer arithmetic and address math. */
void mc_add_rr(struct mc *m, int d, int s) { x_add_rr(m->c, d, s); }
void mc_sub_rr(struct mc *m, int d, int s) { x_sub_rr(m->c, d, s); }
void mc_and_rr(struct mc *m, int d, int s) { x_and_rr(m->c, d, s); }
void mc_or_rr(struct mc *m, int d, int s) { x_or_rr(m->c, d, s); }
void mc_xor_rr(struct mc *m, int d, int s) { x_xor_rr(m->c, d, s); }
void mc_imul_rr(struct mc *m, int d, int s) { x_imul_rr(m->c, d, s); }
void mc_neg_r(struct mc *m, int r) { x_neg_r(m->c, r); }
void mc_not_r(struct mc *m, int r) { x_not_r(m->c, r); }

void mc_add_ri32(struct mc *m, int r, int32_t v) { x_add_ri32(m->c, r, v); }
void mc_sub_ri32(struct mc *m, int r, int32_t v) { x_sub_ri32(m->c, r, v); }

/* Division. */
void mc_cqo(struct mc *m) { x_cqo(m->c); }
void mc_cdq(struct mc *m) { x_cdq(m->c); }
void mc_idiv_r(struct mc *m, int r) { x_idiv_r(m->c, r); }
void mc_idiv32_r(struct mc *m, int r) { x_idiv32_r(m->c, r); }
void mc_div_r(struct mc *m, int r) { x_div_r(m->c, r); }
void mc_div32_r(struct mc *m, int r) { x_div32_r(m->c, r); }

/* Shifts by cl. */
void mc_shl_cl(struct mc *m, int r) { x_shl_cl(m->c, r); }
void mc_sar_cl(struct mc *m, int r) { x_sar_cl(m->c, r); }
void mc_shr_cl(struct mc *m, int r) { x_shr_cl(m->c, r); }
void mc_shl32_cl(struct mc *m, int r) { x_shl32_cl(m->c, r); }
void mc_sar32_cl(struct mc *m, int r) { x_sar32_cl(m->c, r); }
void mc_shr32_cl(struct mc *m, int r) { x_shr32_cl(m->c, r); }

/* Compare and condition materialization. */
void mc_cmp_rr(struct mc *m, int a, int b) { x_cmp_rr(m->c, a, b); }
void mc_test_rr(struct mc *m, int a, int b) { x_test_rr(m->c, a, b); }
void mc_cmp32_rr(struct mc *m, int a, int b) { x_cmp32_rr(m->c, a, b); }
void mc_test32_rr(struct mc *m, int a, int b) { x_test32_rr(m->c, a, b); }
void mc_setcc_r(struct mc *m, int cc, int r) { x_setcc_r(m->c, cc, r); }
void mc_movzx_rb(struct mc *m, int d, int s) { x_movzx_rb(m->c, d, s); }
void mc_movsxd_rr(struct mc *m, int d, int s) { x_movsxd_rr(m->c, d, s); }

/* Memory. */
void mc_load8(struct mc *m, int d, int b, int32_t o) { x_load8(m->c, d, b, o); }
void mc_load8s(struct mc *m, int d, int b, int32_t o) { x_load8s(m->c, d, b, o); }
void mc_load16(struct mc *m, int d, int b, int32_t o) { x_load16(m->c, d, b, o); }
void mc_load16s(struct mc *m, int d, int b, int32_t o) { x_load16s(m->c, d, b, o); }
void mc_store8(struct mc *m, int b, int32_t o, int s) { x_store8(m->c, b, o, s); }
void mc_store16(struct mc *m, int b, int32_t o, int s) { x_store16(m->c, b, o, s); }
void mc_load32(struct mc *m, int d, int b, int32_t o) { x_load32(m->c, d, b, o); }
void mc_store32(struct mc *m, int b, int32_t o, int s) { x_store32(m->c, b, o, s); }
void mc_load64(struct mc *m, int d, int b, int32_t o) { x_load64(m->c, d, b, o); }
void mc_store64(struct mc *m, int b, int32_t o, int s) { x_store64(m->c, b, o, s); }
void mc_lea(struct mc *m, int d, int b, int32_t o) { x_lea(m->c, d, b, o); }

/* Stack. */
void mc_push_r(struct mc *m, int r) { x_push_r(m->c, r); }
void mc_pop_r(struct mc *m, int r) { x_pop_r(m->c, r); }

/* Plain control flow. */
void mc_ret(struct mc *m) { x_ret(m->c); }
void mc_leave(struct mc *m) { x_leave(m->c); }
void mc_nop(struct mc *m) { x_nop(m->c); }
void mc_call_reg(struct mc *m, int r) { x_call_r(m->c, r); }
void mc_jmp_reg(struct mc *m, int r) { x_jmp_r(m->c, r); }

/* Call a named function.  A function defined in this program is a direct rel32
   with a deferred relocation; a runtime symbol is a host binding whose address
   is loaded into rax and called through. */
void
mc_call_sym(struct mc *m, const char *name)
{
    if (is_module_func(m->e->prog, name)) {
        size_t site = x_call_rel32(m->c);
        add_reloc(m->e, site, name);
        return;
    }
    void *a = import_addr(m->e, name);
    if (!a) { m->errsym = name; return; }
    x_mov_ri64(m->c, X_RAX, (uint64_t)(uintptr_t)a);
    x_call_r(m->c, X_RAX);
}

/* Load the address of a data global or a function's code into `reg`.  A global
   is resolved to its mapped address; a function's code address is not known
   until the code is mapped, so emit a placeholder and record a fixup. */
void
mc_lea_sym(struct mc *m, int reg, const char *name)
{
    uint8_t *ga = global_addr(m->e->j, name);
    if (ga) {
        x_mov_ri64(m->c, reg, (uint64_t)(uintptr_t)ga);
    } else if (is_module_func(m->e->prog, name)) {
        x_mov_ri64(m->c, reg, 0);
        add_absreloc(m->e, m->c->len - 8, name);
    } else {
        m->errsym = name;
    }
}

static void
mc_trap(struct mc *m, const char *name)
{
    void *a = import_addr(m->e, name);
    if (!a) { m->errsym = name; return; }
    x_mov_ri64(m->c, X_RAX, (uint64_t)(uintptr_t)a);
    x_call_r(m->c, X_RAX);          /* never returns; raises the fault */
}

void mc_trap_div_zero(struct mc *m) { mc_trap(m, "kp_trap_div_zero"); }
void mc_trap_overflow(struct mc *m) { mc_trap(m, "kp_trap_overflow"); }

void mc_loc(struct mc *m, int line) { add_line(m->e, m->c->len, line); }

/* Double-precision SSE. */
void mc_movsd_rr(struct mc *m, int d, int s) { x_movsd_rr(m->c, d, s); }
void mc_movsd_load(struct mc *m, int d, int b, int32_t o) { x_movsd_load(m->c, d, b, o); }
void mc_movsd_store(struct mc *m, int b, int32_t o, int s) { x_movsd_store(m->c, b, o, s); }
void mc_addsd_rr(struct mc *m, int d, int s) { x_addsd_rr(m->c, d, s); }
void mc_subsd_rr(struct mc *m, int d, int s) { x_subsd_rr(m->c, d, s); }
void mc_mulsd_rr(struct mc *m, int d, int s) { x_mulsd_rr(m->c, d, s); }
void mc_divsd_rr(struct mc *m, int d, int s) { x_divsd_rr(m->c, d, s); }
void mc_ucomisd_rr(struct mc *m, int a, int b) { x_ucomisd_rr(m->c, a, b); }
void mc_cvtsi2sd(struct mc *m, int d, int s) { x_cvtsi2sd(m->c, d, s); }
void mc_sqrtsd_rr(struct mc *m, int d, int s) { x_sqrtsd_rr(m->c, d, s); }
void mc_cvttsd2si(struct mc *m, int d, int s) { x_cvttsd2si(m->c, d, s); }
void mc_cvtsd2si(struct mc *m, int d, int s) { x_cvtsd2si(m->c, d, s); }

/* Single-precision SSE and the width converts. */
void mc_movss_load(struct mc *m, int d, int b, int32_t o) { x_movss_load(m->c, d, b, o); }
void mc_movss_store(struct mc *m, int b, int32_t o, int s) { x_movss_store(m->c, b, o, s); }
void mc_addss_rr(struct mc *m, int d, int s) { x_addss_rr(m->c, d, s); }
void mc_subss_rr(struct mc *m, int d, int s) { x_subss_rr(m->c, d, s); }
void mc_mulss_rr(struct mc *m, int d, int s) { x_mulss_rr(m->c, d, s); }
void mc_divss_rr(struct mc *m, int d, int s) { x_divss_rr(m->c, d, s); }
void mc_ucomiss_rr(struct mc *m, int a, int b) { x_ucomiss_rr(m->c, a, b); }
void mc_cvtsi2ss(struct mc *m, int d, int s) { x_cvtsi2ss(m->c, d, s); }
void mc_cvttss2si(struct mc *m, int d, int s) { x_cvttss2si(m->c, d, s); }
void mc_movd_from_xmm(struct mc *m, int d, int s) { x_movd_from_xmm(m->c, d, s); }
void mc_movd_to_xmm(struct mc *m, int d, int s) { x_movd_to_xmm(m->c, d, s); }

/* Inline assembly has no byte-sink form: the JIT emits machine code directly and
   has no downstream assembler for a verbatim string, so report it unsupported.
   The cc JIT suite excludes the inline-asm test, so this is never reached. */
void mc_asm(struct mc *m, const char *text) { m->errsym = text; }
void mc_cvtss2sd_rr(struct mc *m, int d, int s) { x_cvtss2sd_rr(m->c, d, s); }
void mc_cvtsd2ss_rr(struct mc *m, int d, int s) { x_cvtsd2ss_rr(m->c, d, s); }
void mc_cvtss2sd_load(struct mc *m, int d, int b, int32_t o) { x_cvtss2sd_load(m->c, d, b, o); }
void mc_movq_from_xmm(struct mc *m, int d, int s) { x_movq_from_xmm(m->c, d, s); }
void mc_movq_to_xmm(struct mc *m, int d, int s) { x_movq_to_xmm(m->c, d, s); }

/* Emit one function's body: set up the byte sink over the code buffer, run the
   shared x86-64 selector through it, and surface either its unsupported-opcode
   error or a symbol the sink could not resolve. */
static int
emit_func(struct emitctx *e, struct code *c, struct ir_func *fn)
{
    struct mc M;
    char errbuf[128];
    int rc;

    mc_reset(&M, c, e);
    rc = x86_select_func(&M, fn, TRAP_GUARDED, errbuf, sizeof errbuf);
    if (rc == 0 && M.errsym)
        rc = fail(e, "unresolved symbol '%s'", M.errsym, 0);
    else if (rc != 0)
        rc = fail(e, "%s", errbuf, 0);
    mc_free(&M);
    return rc;
}

/****************************************************************
 * Data region
 ****************************************************************/

static int
layout_data(struct kp_jit *j, struct ir_program *prog, char *err, size_t errlen)
{
    int ng = 0;
    size_t off = 0;

    for (struct ir_global *g = prog->globals; g; g = g->next)
        ng++;
    j->globals = calloc((size_t)(ng > 0 ? ng : 1), sizeof(*j->globals));
    j->nglobals = ng;

    /* assign 8-aligned offsets */
    int i = 0;
    for (struct ir_global *g = prog->globals; g; g = g->next, i++) {
        size_t sz = (size_t)(g->arr_size > 0 ? g->arr_size : 1)
                    * (size_t)base_size(g->base_type);
        off = (off + 7) & ~(size_t)7;
        j->globals[i].name = g->name;
        j->globals[i].addr = (uint8_t *)off;   /* provisional: byte offset */
        off += sz;
    }
    j->data_len = off;
    j->data = NULL;
    if (off) {
        /* Map the data region in the low 2GB so a global's address fits in
           the 32 bits the front end models addresses with; a receiver or
           array pointer then round-trips through its i32 slot. Anonymous
           pages come back zeroed, which is the default initializer. */
        long pagesz = sysconf(_SC_PAGESIZE);
        j->data_map = ((off + (size_t)pagesz - 1) / (size_t)pagesz)
                      * (size_t)pagesz;
        j->data = mmap(NULL, j->data_map, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
        if (j->data == MAP_FAILED) {
            j->data = NULL;
            snprintf(err, errlen, "mmap for data region failed");
            return -1;
        }
    }
    /* rebase provisional offsets to real addresses, then initialize */
    i = 0;
    for (struct ir_global *g = prog->globals; g; g = g->next, i++) {
        uint8_t *a = j->data + (size_t)j->globals[i].addr;
        j->globals[i].addr = a;
        if (g->inits) {
            /* An aggregate byte image (structs, designated/mixed initializers).
               Gaps stay zero from the anonymous mapping.  A scalar entry is
               written here little-endian; a symbol reference (it->sym) is a
               relocation resolved after code layout (see kp_jit). */
            for (struct ir_init *it = g->inits; it; it = it->next) {
                if (it->sym)
                    continue;
                memcpy(a + (size_t)it->offset, &it->ival, (size_t)it->size);
            }
        } else if (g->init_string) {
            memcpy(a, g->init_string, (size_t)g->init_strlen);
        } else if (g->init_count > 0) {
            int es = base_size(g->base_type);
            for (int k = 0; k < g->init_count; k++)
                memcpy(a + (size_t)k * es, &g->init_ivals[k], (size_t)es);
        }
    }
    return 0;
}

/****************************************************************
 * Public API
 ****************************************************************/

int
kp_jit(struct kp_jit *j, struct ir_program *prog,
       const struct kp_binding *binds, int nbinds,
       char *err, size_t errlen)
{
    struct emitctx e;
    struct code c;
    int nf = 0, i, rc = 0;
    long pagesz;

    memset(j, 0, sizeof(*j));
    if (err && errlen)
        err[0] = '\0';

    if (layout_data(j, prog, err, errlen) != 0)
        return -1;

    memset(&e, 0, sizeof(e));
    e.j = j;
    e.prog = prog;
    e.binds = binds;
    e.nbinds = nbinds;
    e.err = err;
    e.errlen = errlen;

    for (struct ir_func *fn = prog->funcs; fn; fn = fn->next)
        nf++;
    j->funcs = calloc((size_t)(nf > 0 ? nf : 1), sizeof(*j->funcs));
    j->nfuncs = nf;

    code_init(&c);
    i = 0;
    for (struct ir_func *fn = prog->funcs; fn; fn = fn->next, i++) {
        regalloc(fn);
        j->funcs[i].name = fn->name;
        j->funcs[i].off = c.len;
        if (emit_func(&e, &c, fn) != 0) {
            rc = -1;
            break;
        }
    }

    /* Hand the address-to-line table (and the source file it indexes) to the
       jit, which now owns e.lines; kp_jit_free releases it on any later error. */
    j->lines = e.lines;
    j->nlines = e.nlines;
    j->source_file = NULL;   /* -g line info not wired to skj-jit yet */
    e.lines = NULL;

    /* resolve deferred intra-module calls */
    for (int r = 0; r < e.nrelocs && rc == 0; r++) {
        size_t target = 0;
        int found = 0;
        for (int k = 0; k < j->nfuncs; k++)
            if (strcmp(j->funcs[k].name, e.relocs[r].target) == 0) {
                target = j->funcs[k].off;
                found = 1;
                break;
            }
        if (!found)
            rc = fail(&e, "call to undefined function '%s'", e.relocs[r].target, 0);
        else
            x_patch_rel32(&c, e.relocs[r].site, target);
    }
    free(e.relocs);

    if (rc != 0) {
        free(e.absrelocs);
        code_free(&c);
        kp_jit_free(j);
        return -1;
    }

    /* copy the finished code into a read-execute mapping */
    pagesz = sysconf(_SC_PAGESIZE);
    j->code_len = c.len;
    j->code_map = ((c.len + (size_t)pagesz - 1) / (size_t)pagesz) * (size_t)pagesz;
    if (j->code_map == 0)
        j->code_map = (size_t)pagesz;
    /* map code in the low 2GB too, so a function's address fits the 32 bits
       a procedural value is stored in */
    j->code = mmap(NULL, j->code_map, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (j->code == MAP_FAILED) {
        j->code = NULL;
        free(e.absrelocs);
        code_free(&c);
        kp_jit_free(j);
        snprintf(err, errlen, "mmap for code failed");
        return -1;
    }
    memcpy(j->code, c.buf, c.len);
    code_free(&c);
    /* patch function code pointers now that the code base is known */
    for (int r = 0; r < e.nabs; r++) {
        uint64_t addr = 0;
        for (int k = 0; k < j->nfuncs; k++)
            if (strcmp(j->funcs[k].name, e.absrelocs[r].target) == 0) {
                addr = (uint64_t)(uintptr_t)(j->code + j->funcs[k].off);
                break;
            }
        memcpy(j->code + e.absrelocs[r].site, &addr, sizeof(addr));
    }
    free(e.absrelocs);

    /* Data relocations: a global initialized with the address of another
       symbol (a pointer global, a function-pointer table).  The target address
       is known only now that data and code are laid out, so patch the image
       here.  Resolve against the globals, then the functions, then the host
       bindings, and write the low bytes (a 32-bit address in the ILP32 model). */
    {
        int gi = 0;
        for (struct ir_global *g = prog->globals; g; g = g->next, gi++) {
            uint8_t *base = j->globals[gi].addr;
            uint64_t addr;

            /* aggregate image: a symbol reference at a byte offset */
            for (struct ir_init *it = g->inits; it; it = it->next) {
                if (!it->sym)
                    continue;
                if (!resolve_data_sym(j, binds, nbinds, it->sym, &addr)) {
                    snprintf(err, errlen,
                             "data init references undefined symbol '%s'", it->sym);
                    kp_jit_free(j);
                    return -1;
                }
                memcpy(base + it->offset, &addr, (size_t)it->size);
            }
            /* flat init: init_syms[k] parallels init_ivals[k] (a scalar pointer
               global, or an array of addresses).  cc uses this rather than the
               inits list for a plain `T *p = &sym;`. */
            if (g->init_count > 0 && g->init_syms) {
                int es = base_size(g->base_type);
                for (int k = 0; k < g->init_count; k++) {
                    if (!g->init_syms[k])
                        continue;
                    if (!resolve_data_sym(j, binds, nbinds, g->init_syms[k], &addr)) {
                        snprintf(err, errlen,
                                 "data init references undefined symbol '%s'",
                                 g->init_syms[k]);
                        kp_jit_free(j);
                        return -1;
                    }
                    memcpy(base + (size_t)k * es, &addr, (size_t)es);
                }
            }
        }
    }

    if (mprotect(j->code, j->code_map, PROT_READ | PROT_EXEC) != 0) {
        kp_jit_free(j);
        snprintf(err, errlen, "mprotect read-exec failed");
        return -1;
    }

    /* a 1 MB execution stack in the low 2GB, so every frame address fits the
       32 bits the front end models addresses with */
    j->stack_len = 1u << 20;
    j->stack = mmap(NULL, j->stack_len, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (j->stack == MAP_FAILED) {
        j->stack = NULL;
        kp_jit_free(j);
        snprintf(err, errlen, "mmap for stack failed");
        return -1;
    }
    return 0;
}

void
kp_jit_call(struct kp_jit *j, void *entry)
{
    uintptr_t top = ((uintptr_t)j->stack + j->stack_len) & ~(uintptr_t)15;

    /* Save rsp in a callee-saved register (entry preserves it per the SysV
       ABI), switch to the low stack 16-byte aligned at the call, then
       restore. entry takes no arguments and returns nothing here. */
    __asm__ volatile (
        "mov %%rsp, %%rbx\n\t"
        "mov %0, %%rsp\n\t"
        "call *%1\n\t"
        "mov %%rbx, %%rsp\n\t"
        :
        : "r"(top), "r"(entry)
        : "rbx", "rax", "rcx", "rdx", "rsi", "rdi",
          "r8", "r9", "r10", "r11", "memory", "cc");
}

void *
kp_jit_entry(struct kp_jit *j, const char *name)
{
    for (int i = 0; i < j->nfuncs; i++)
        if (strcmp(j->funcs[i].name, name) == 0)
            return j->code + j->funcs[i].off;
    return NULL;
}

void
kp_jit_free(struct kp_jit *j)
{
    if (j->code)
        munmap(j->code, j->code_map);
    if (j->data)
        munmap(j->data, j->data_map);
    if (j->stack)
        munmap(j->stack, j->stack_len);
    free(j->globals);
    free(j->funcs);
    free(j->lines);
    memset(j, 0, sizeof(*j));
}
