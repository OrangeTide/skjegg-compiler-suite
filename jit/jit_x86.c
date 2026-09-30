/* jit_x86.c : the x86-64 target of the in-process JIT.

   The byte sink: it implements the backend/mc.h operations the shared x86-64
   instruction selector (backend/x86_select.c) emits through, writing machine
   code via the jit/emit_x86.c encoder.  It plugs into the architecture-neutral
   core (jit/jit_common.c) as kp_target: a per-function emit routine plus the
   two relocation appliers whose encodings are x86-specific (a rel32 call, an
   imm64 code pointer).  The container, layout, and kp_jit() driver live in
   jit_common.c; the AArch64 counterpart is jit_arm64.c. */

#define _POSIX_C_SOURCE 200809L

#include "jit_common.h"
#include "emit_x86.h"
#include "mc.h"
#include "x86_select.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/****************************************************************
 * Byte sink: the mc.h machine-code sink over the emit_x86.c encoder.
 *
 * The shared x86-64 selector (backend/x86_select.c) emits through mc_*; this is
 * the implementation that writes machine-code bytes for the in-process JIT.  A
 * struct mc bundles the growable code buffer, the emit context (globals, host
 * bindings, relocations, the line table), and a per-function label table with
 * forward-reference fixups.  The text sink (backend/mc_text.c) is the AOT
 * counterpart.
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
    if (kp_is_module_func(m->e->prog, name)) {
        size_t site = x_call_rel32(m->c);
        kp_add_reloc(m->e, site, name);
        return;
    }
    void *a = kp_import_addr(m->e, name);
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
    uint8_t *ga = kp_global_addr(m->e->j, name);
    if (ga) {
        x_mov_ri64(m->c, reg, (uint64_t)(uintptr_t)ga);
    } else if (kp_is_module_func(m->e->prog, name)) {
        x_mov_ri64(m->c, reg, 0);
        kp_add_absreloc(m->e, m->c->len - 8, name);
    } else {
        m->errsym = name;
    }
}

static void
mc_trap(struct mc *m, const char *name)
{
    void *a = kp_import_addr(m->e, name);
    if (!a) { m->errsym = name; return; }
    x_mov_ri64(m->c, X_RAX, (uint64_t)(uintptr_t)a);
    x_call_r(m->c, X_RAX);          /* never returns; raises the fault */
}

void mc_trap_div_zero(struct mc *m) { mc_trap(m, "kp_trap_div_zero"); }
void mc_trap_overflow(struct mc *m) { mc_trap(m, "kp_trap_overflow"); }

void mc_loc(struct mc *m, int line) { kp_add_line(m->e, m->c->len, line); }

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

/****************************************************************
 * kp_target: the x86-64 back end kp_jit() drives
 ****************************************************************/

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
        rc = kp_fail(e, "unresolved symbol '%s'", M.errsym, 0);
    else if (rc != 0)
        rc = kp_fail(e, "%s", errbuf, 0);
    mc_free(&M);
    return rc;
}

/* A deferred intra-module call is a rel32 patched relative to the site.  The
   JIT code region is far under 2GB, so a rel32 always fits: always returns 0. */
static int
x86_apply_call_reloc(struct code *c, size_t site, size_t target)
{
    x_patch_rel32(c, site, target);
    return 0;
}

/* An absolute function-code reference is a full imm64 written into the code. */
static void
x86_apply_abs_reloc(uint8_t *code, size_t site, uint64_t addr)
{
    memcpy(code + site, &addr, sizeof(addr));
}

const struct jit_target kp_target = {
    emit_func,
    x86_apply_call_reloc,
    x86_apply_abs_reloc,
};

/****************************************************************
 * Running a compiled entry
 ****************************************************************/

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
