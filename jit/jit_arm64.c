/* jit_arm64.c : the AArch64 target of the in-process JIT.

   The byte sink: it implements the backend/arm64_mc.h operations the shared
   AArch64 selector (backend/arm64_select.c) emits through, writing machine code
   via the jit/emit_arm64.c encoder.  It plugs into the architecture-neutral core
   (jit/jit_common.c) as kp_target: a per-function emit routine plus the two
   relocation appliers whose encodings are AArch64-specific (a BL word offset, a
   movz/movk code pointer).  The x86-64 counterpart is jit_x86.c. */

#define _POSIX_C_SOURCE 200809L

#include "jit_common.h"
#include "emit_arm64.h"
#include "arm64_mc.h"
#include "arm64_select.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/****************************************************************
 * Byte sink over the emit_arm64.c encoder
 *
 * A struct mc64 bundles the growable code buffer, the emit context (globals,
 * host bindings, relocations, the line table), and a per-function label table
 * with forward-reference fixups.  A branch fixup records whether the site holds
 * an imm26 (b/bl) or an imm19 (cbz/cbnz) field.
 ****************************************************************/

struct mc64fix {
    size_t site;
    int imm19;              /* 1 = 19-bit field (cbz/cbnz), 0 = 26-bit (b) */
};

struct mc64lab {
    long off;
    struct mc64fix *fix;
    int nfix, cfix;
};

struct mc64 {
    struct code *c;
    struct emitctx *e;
    struct mc64lab *lab;
    int nlab, clab;
    const char *errsym;
};

#define SCR 15                  /* the address/immediate scratch (x15) */
#define SCC 16                  /* the call-target/host-address scratch (x16) */

static void
mc64_reset(struct mc64 *m, struct code *c, struct emitctx *e)
{
    m->c = c;
    m->e = e;
    m->lab = NULL;
    m->nlab = m->clab = 0;
    m->errsym = NULL;
}

static void
mc64_free_sink(struct mc64 *m)
{
    for (int i = 0; i < m->nlab; i++)
        free(m->lab[i].fix);
    free(m->lab);
    m->lab = NULL;
    m->nlab = m->clab = 0;
}

int
mc64_new_label(struct mc64 *m)
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

static int
patch(struct code *c, struct mc64fix *f, size_t target)
{
    return f->imm19 ? a_patch_imm19(c, f->site, target)
                    : a_patch_imm26(c, f->site, target);
}

void
mc64_label(struct mc64 *m, int id)
{
    struct mc64lab *l = &m->lab[id];
    l->off = (long)m->c->len;
    for (int i = 0; i < l->nfix; i++)
        if (patch(m->c, &l->fix[i], (size_t)l->off) != 0)
            m->errsym = "branch target out of range";
    free(l->fix);
    l->fix = NULL;
    l->nfix = l->cfix = 0;
}

static void
mc64_ref(struct mc64 *m, int id, size_t site, int imm19)
{
    struct mc64lab *l = &m->lab[id];
    if (l->off >= 0) {
        struct mc64fix f = { site, imm19 };
        if (patch(m->c, &f, (size_t)l->off) != 0)
            m->errsym = "branch target out of range";
        return;
    }
    if (l->nfix == l->cfix) {
        l->cfix = l->cfix ? l->cfix * 2 : 4;
        l->fix = realloc(l->fix, (size_t)l->cfix * sizeof(*l->fix));
    }
    l->fix[l->nfix].site = site;
    l->fix[l->nfix].imm19 = imm19;
    l->nfix++;
}

void mc64_b(struct mc64 *m, int id) { mc64_ref(m, id, a_b(m->c), 0); }
void mc64_cbz(struct mc64 *m, int sf, int Rt, int id) { mc64_ref(m, id, a_cbz(m->c, sf, Rt), 1); }
void mc64_cbnz(struct mc64 *m, int sf, int Rt, int id) { mc64_ref(m, id, a_cbnz(m->c, sf, Rt), 1); }
void mc64_ret(struct mc64 *m) { a_ret(m->c); }
void mc64_blr(struct mc64 *m, int Rn) { a_blr(m->c, Rn); }
void mc64_br(struct mc64 *m, int Rn) { a_br(m->c, Rn); }

/* Moves / immediates / addresses. */
void mc64_mov(struct mc64 *m, int sf, int Rd, int Rn) { a_mov_reg(m->c, sf, Rd, Rn); }
void mc64_movimm(struct mc64 *m, int sf, int Rd, uint64_t v) { a_movimm(m->c, sf, Rd, v); }

void
mc64_lea_sym(struct mc64 *m, int sf, int Rd, const char *name)
{
    uint8_t *ga = kp_global_addr(m->e->j, name);
    if (ga) {
        a_movimm(m->c, sf, Rd, (uint64_t)(uintptr_t)ga);
    } else if (kp_is_module_func(m->e->prog, name)) {
        /* the code base is not known yet: emit a fixed movz/movk pair (a
           low-memory <4GB address needs only the low two 16-bit lanes) and
           record a fixup patched once the code is mapped. */
        size_t site = m->c->len;
        a_movz(m->c, sf, Rd, 0, 0);
        a_movk(m->c, sf, Rd, 0, 16);
        kp_add_absreloc(m->e, site, name);
    } else {
        /* an external data symbol the host provides (the Excelsior runtime's
           __exc_self, shared between guest code and host libexc).  The driver
           is linked so this host address is < 4GB, which the guest loads into
           a 32-bit register. */
        void *ia = kp_import_addr(m->e, name);
        if (ia)
            a_movimm(m->c, sf, Rd, (uint64_t)(uintptr_t)ia);
        else
            m->errsym = name;
    }
}

void mc64_addimm(struct mc64 *m, int Rd, int Rn, long imm) { a_addimm(m->c, Rd, Rn, imm, SCR); }

/* Integer arithmetic and logic. */
void mc64_add(struct mc64 *m, int sf, int Rd, int Rn, int Rm) { a_add_reg(m->c, sf, Rd, Rn, Rm); }
void mc64_sub(struct mc64 *m, int sf, int Rd, int Rn, int Rm) { a_sub_reg(m->c, sf, Rd, Rn, Rm); }
void mc64_and(struct mc64 *m, int sf, int Rd, int Rn, int Rm) { a_and_reg(m->c, sf, Rd, Rn, Rm); }
void mc64_orr(struct mc64 *m, int sf, int Rd, int Rn, int Rm) { a_orr_reg(m->c, sf, Rd, Rn, Rm); }
void mc64_eor(struct mc64 *m, int sf, int Rd, int Rn, int Rm) { a_eor_reg(m->c, sf, Rd, Rn, Rm); }
void mc64_mul(struct mc64 *m, int sf, int Rd, int Rn, int Rm) { a_mul(m->c, sf, Rd, Rn, Rm); }
void mc64_sdiv(struct mc64 *m, int sf, int Rd, int Rn, int Rm) { a_sdiv(m->c, sf, Rd, Rn, Rm); }
void mc64_udiv(struct mc64 *m, int sf, int Rd, int Rn, int Rm) { a_udiv(m->c, sf, Rd, Rn, Rm); }
void mc64_lsl(struct mc64 *m, int sf, int Rd, int Rn, int Rm) { a_lslv(m->c, sf, Rd, Rn, Rm); }
void mc64_asr(struct mc64 *m, int sf, int Rd, int Rn, int Rm) { a_asrv(m->c, sf, Rd, Rn, Rm); }
void mc64_lsr(struct mc64 *m, int sf, int Rd, int Rn, int Rm) { a_lsrv(m->c, sf, Rd, Rn, Rm); }
void mc64_msub(struct mc64 *m, int sf, int Rd, int Rn, int Rm, int Ra) { a_msub(m->c, sf, Rd, Rn, Rm, Ra); }
void mc64_neg(struct mc64 *m, int sf, int Rd, int Rm) { a_neg(m->c, sf, Rd, Rm); }
void mc64_mvn(struct mc64 *m, int sf, int Rd, int Rm) { a_mvn(m->c, sf, Rd, Rm); }
void mc64_sxtw(struct mc64 *m, int Rd, int Rn) { a_sxtw(m->c, Rd, Rn); }
void mc64_cmp(struct mc64 *m, int sf, int Rn, int Rm) { a_cmp_reg(m->c, sf, Rn, Rm); }
void mc64_cset(struct mc64 *m, int sf, int Rd, int cc) { a_cset(m->c, sf, Rd, cc); }

/****************************************************************
 * Loads and stores, with a scratch-base fallback for a large offset
 ****************************************************************/

/* True when off encodes directly (unscaled signed 9-bit, or scaled unsigned
   12-bit for the access size); otherwise materialise base+off through x15. */
static int
off_fits(int off, int scale)
{
    return (off >= -256 && off <= 255) ||
           (off >= 0 && (off % scale) == 0 && (off / scale) <= 0xfff);
}

typedef void (*gpr_ls)(struct code *, int, int, int);

static void
gpr_mem(struct mc64 *m, gpr_ls op, int scale, int Rt, int base, int off)
{
    if (off_fits(off, scale)) {
        op(m->c, Rt, base, off);
    } else {
        a_addimm(m->c, SCR, base, off, SCR);
        op(m->c, Rt, SCR, 0);
    }
}

void mc64_ldrb(struct mc64 *m, int Rt, int base, int off) { gpr_mem(m, a_ldrb, 1, Rt, base, off); }
void mc64_ldrsb(struct mc64 *m, int Rt, int base, int off) { gpr_mem(m, a_ldrsb, 1, Rt, base, off); }
void mc64_ldrh(struct mc64 *m, int Rt, int base, int off) { gpr_mem(m, a_ldrh, 2, Rt, base, off); }
void mc64_ldrsh(struct mc64 *m, int Rt, int base, int off) { gpr_mem(m, a_ldrsh, 2, Rt, base, off); }
void mc64_ldrw(struct mc64 *m, int Rt, int base, int off) { gpr_mem(m, a_ldrw, 4, Rt, base, off); }
void mc64_ldrx(struct mc64 *m, int Rt, int base, int off) { gpr_mem(m, a_ldrx, 8, Rt, base, off); }
void mc64_strb(struct mc64 *m, int Rt, int base, int off) { gpr_mem(m, a_strb, 1, Rt, base, off); }
void mc64_strh(struct mc64 *m, int Rt, int base, int off) { gpr_mem(m, a_strh, 2, Rt, base, off); }
void mc64_strw(struct mc64 *m, int Rt, int base, int off) { gpr_mem(m, a_strw, 4, Rt, base, off); }
void mc64_strx(struct mc64 *m, int Rt, int base, int off) { gpr_mem(m, a_strx, 8, Rt, base, off); }

void
mc64_ldr_fp(struct mc64 *m, int sz, int Rt, int base, int off)
{
    if (off_fits(off, 1 << sz)) {
        a_ldr_fp(m->c, sz, Rt, base, off);
    } else {
        a_addimm(m->c, SCR, base, off, SCR);
        a_ldr_fp(m->c, sz, Rt, SCR, 0);
    }
}

void
mc64_str_fp(struct mc64 *m, int sz, int Rt, int base, int off)
{
    if (off_fits(off, 1 << sz)) {
        a_str_fp(m->c, sz, Rt, base, off);
    } else {
        a_addimm(m->c, SCR, base, off, SCR);
        a_str_fp(m->c, sz, Rt, SCR, 0);
    }
}

/* FP data processing. */
void mc64_fmov(struct mc64 *m, int sz, int Rd, int Rn) { a_fmov(m->c, sz, Rd, Rn); }
void mc64_fadd(struct mc64 *m, int sz, int Rd, int Rn, int Rm) { a_fadd(m->c, sz, Rd, Rn, Rm); }
void mc64_fsub(struct mc64 *m, int sz, int Rd, int Rn, int Rm) { a_fsub(m->c, sz, Rd, Rn, Rm); }
void mc64_fmul(struct mc64 *m, int sz, int Rd, int Rn, int Rm) { a_fmul(m->c, sz, Rd, Rn, Rm); }
void mc64_fdiv(struct mc64 *m, int sz, int Rd, int Rn, int Rm) { a_fdiv(m->c, sz, Rd, Rn, Rm); }
void mc64_fneg(struct mc64 *m, int sz, int Rd, int Rn) { a_fneg(m->c, sz, Rd, Rn); }
void mc64_fabs(struct mc64 *m, int sz, int Rd, int Rn) { a_fabs(m->c, sz, Rd, Rn); }
void mc64_fsqrt(struct mc64 *m, int sz, int Rd, int Rn) { a_fsqrt(m->c, sz, Rd, Rn); }
void mc64_fcmp(struct mc64 *m, int sz, int Rn, int Rm) { a_fcmp(m->c, sz, Rn, Rm); }
void mc64_fcvt(struct mc64 *m, int dstsz, int srcsz, int Rd, int Rn) { a_fcvt(m->c, dstsz, srcsz, Rd, Rn); }
void mc64_scvtf(struct mc64 *m, int sz, int Rd, int Rn) { a_scvtf(m->c, sz, Rd, Rn); }
void mc64_fcvtzs(struct mc64 *m, int sz, int Rd, int Rn) { a_fcvtzs(m->c, sz, Rd, Rn); }
void mc64_fcvtns(struct mc64 *m, int sz, int Rd, int Rn) { a_fcvtns(m->c, sz, Rd, Rn); }

/****************************************************************
 * Calls, traps, markers, inline asm
 ****************************************************************/

/* A module function is a direct BL with a deferred relocation; a runtime symbol
   is a host binding whose address is loaded into x16 and called through. */
void
mc64_call_sym(struct mc64 *m, const char *name)
{
    if (kp_is_module_func(m->e->prog, name)) {
        size_t site = a_bl(m->c);
        kp_add_reloc(m->e, site, name);
        return;
    }
    void *a = kp_import_addr(m->e, name);
    if (!a) { m->errsym = name; return; }
    a_movimm(m->c, 1, SCC, (uint64_t)(uintptr_t)a);
    a_blr(m->c, SCC);
}

static void
mc64_trap(struct mc64 *m, const char *name)
{
    void *a = kp_import_addr(m->e, name);
    if (!a) { m->errsym = name; return; }
    a_movimm(m->c, 1, SCC, (uint64_t)(uintptr_t)a);
    a_blr(m->c, SCC);               /* never returns; raises the fault */
}

void mc64_trap_div_zero(struct mc64 *m) { mc64_trap(m, "kp_trap_div_zero"); }
void mc64_trap_overflow(struct mc64 *m) { mc64_trap(m, "kp_trap_overflow"); }

void mc64_loc(struct mc64 *m, int line) { kp_add_line(m->e, m->c->len, line); }

/* Inline assembly has no byte-sink form: report it unsupported (the cc JIT suite
   excludes the inline-asm test, so this is never reached). */
void mc64_asm(struct mc64 *m, const char *text) { m->errsym = text; }

/****************************************************************
 * kp_target: the AArch64 back end kp_jit() drives
 ****************************************************************/

static int
emit_func(struct emitctx *e, struct code *c, struct ir_func *fn)
{
    struct mc64 M;
    char errbuf[128];
    int rc;

    mc64_reset(&M, c, e);
    rc = arm64_select_func(&M, fn, ARM64_TRAP_GUARDED, errbuf, sizeof errbuf);
    if (rc == 0 && M.errsym)
        rc = kp_fail(e, "unresolved symbol '%s'", M.errsym, 0);
    else if (rc != 0)
        rc = kp_fail(e, "%s", errbuf, 0);
    mc64_free_sink(&M);
    return rc;
}

/* A deferred intra-module call is a BL whose 26-bit word offset is patched;
   -1 if the target is beyond the +/-128MB range. */
static int
arm64_apply_call_reloc(struct code *c, size_t site, size_t target)
{
    return a_patch_imm26(c, site, target);
}

/* An absolute function-code reference is the movz/movk pair mc64_lea_sym emitted:
   patch the low two 16-bit lanes (a low-memory <4GB address). */
static void
arm64_apply_abs_reloc(uint8_t *code, size_t site, uint64_t addr)
{
    uint32_t w0, w1;
    memcpy(&w0, code + site, 4);
    memcpy(&w1, code + site + 4, 4);
    w0 = (w0 & ~(0xffffu << 5)) | (((uint32_t)(addr & 0xffff)) << 5);
    w1 = (w1 & ~(0xffffu << 5)) | (((uint32_t)((addr >> 16) & 0xffff)) << 5);
    memcpy(code + site, &w0, 4);
    memcpy(code + site + 4, &w1, 4);
}

const struct jit_target kp_target = {
    emit_func,
    arm64_apply_call_reloc,
    arm64_apply_abs_reloc,
};

/****************************************************************
 * Running a compiled entry
 ****************************************************************/

void
kp_jit_call(struct kp_jit *j, void *entry)
{
    uintptr_t top = ((uintptr_t)j->stack + j->stack_len) & ~(uintptr_t)15;

    /* Save the host sp in a callee-saved register (entry preserves x19 per the
       AAPCS), switch to the low stack for the call, then restore.  entry takes
       no arguments and returns nothing here. */
    __asm__ volatile (
        "mov x19, sp\n\t"
        "mov sp, %0\n\t"
        "blr %1\n\t"
        "mov sp, x19\n\t"
        :
        : "r"(top), "r"(entry)
        : "x19", "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7",
          "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15",
          "x16", "x17", "x18", "x30", "memory", "cc");
}
