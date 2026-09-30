/* jit_rv.c : the RISC-V RV32 target of the in-process JIT.

   The byte sink: it implements the backend/rv_mc.h operations the shared RISC-V
   selector (backend/rv_select.c) emits through, writing machine code via the
   jit/emit_rv.c encoder.  It plugs into the architecture-neutral core
   (jit/jit_common.c) as kp_target: a per-function emit routine plus the two
   relocation appliers whose encodings are RISC-V-specific (a J-type jal word
   offset, a lui/addi absolute code pointer).  The x86-64 counterpart is
   jit_x86.c and the AArch64 one is jit_arm64.c.

   RV32 is natively 32-bit, so a guest pointer is already a valid host pointer;
   there is no low-memory address juggling.  The RV32 integer branches reach only
   +/-4KB (B-type) or +/-1MB (J-type), which the small JIT code regions stay
   within; an out-of-range target is reported through the sink's error channel
   (the AOT assembler relaxes such branches, but the byte sink does not). */

#define _POSIX_C_SOURCE 200809L

#include "jit_common.h"
#include "emit_rv.h"
#include "rv_mc.h"
#include "rv_select.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/****************************************************************
 * Byte sink over the emit_rv.c encoder
 ****************************************************************/

struct rvfix {
    size_t site;
    int isj;                /* 1 = J-type jal (j), 0 = B-type branch */
};

struct rvlab {
    long off;
    struct rvfix *fix;
    int nfix, cfix;
};

struct rv_mc {
    struct code *c;
    struct emitctx *e;
    struct rvlab *lab;
    int nlab, clab;
    const char *errsym;
};

#define T6 31                   /* the address/immediate scratch (reserved) */

static void
rv_reset(struct rv_mc *m, struct code *c, struct emitctx *e)
{
    m->c = c;
    m->e = e;
    m->lab = NULL;
    m->nlab = m->clab = 0;
    m->errsym = NULL;
}

static void
rv_free_sink(struct rv_mc *m)
{
    for (int i = 0; i < m->nlab; i++)
        free(m->lab[i].fix);
    free(m->lab);
    m->lab = NULL;
    m->nlab = m->clab = 0;
}

/* Load an arbitrary 32-bit constant with a fixed two-instruction lui/addi pair
   (the same split rv_apply_abs_reloc reconstructs). */
static void
li32(struct code *c, int rd, uint32_t v)
{
    int32_t val = (int32_t)v;
    int32_t hi = (val + 0x800) >> 12;
    int32_t lo = val - (hi << 12);
    rv_lui(c, rd, hi & 0xfffff);
    rv_addi(c, rd, rd, lo);
}

/****************************************************************
 * Labels and branch fixups
 ****************************************************************/

int
rv_mc_new_label(struct rv_mc *m)
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
patch(struct code *c, struct rvfix *f, size_t target)
{
    int32_t disp = (int32_t)((long)target - (long)f->site);
    return f->isj ? rv_patch_j(c, f->site, disp) : rv_patch_b(c, f->site, disp);
}

void
rv_mc_label(struct rv_mc *m, int id)
{
    struct rvlab *l = &m->lab[id];
    l->off = (long)m->c->len;
    for (int i = 0; i < l->nfix; i++)
        if (patch(m->c, &l->fix[i], (size_t)l->off) != 0)
            m->errsym = "branch target out of range";
    free(l->fix);
    l->fix = NULL;
    l->nfix = l->cfix = 0;
}

static void
rv_ref(struct rv_mc *m, int id, size_t site, int isj)
{
    struct rvlab *l = &m->lab[id];
    if (l->off >= 0) {
        struct rvfix f = { site, isj };
        if (patch(m->c, &f, (size_t)l->off) != 0)
            m->errsym = "branch target out of range";
        return;
    }
    if (l->nfix == l->cfix) {
        l->cfix = l->cfix ? l->cfix * 2 : 4;
        l->fix = realloc(l->fix, (size_t)l->cfix * sizeof(*l->fix));
    }
    l->fix[l->nfix].site = site;
    l->fix[l->nfix].isj = isj;
    l->nfix++;
}

void rv_mc_j(struct rv_mc *m, int id) { size_t s = m->c->len; rv_jal(m->c, 0, 0); rv_ref(m, id, s, 1); }
void rv_mc_beqz(struct rv_mc *m, int rs, int id) { size_t s = m->c->len; rv_beq(m->c, rs, 0, 0); rv_ref(m, id, s, 0); }
void rv_mc_bnez(struct rv_mc *m, int rs, int id) { size_t s = m->c->len; rv_bne(m->c, rs, 0, 0); rv_ref(m, id, s, 0); }
void rv_mc_bne(struct rv_mc *m, int rs1, int rs2, int id) { size_t s = m->c->len; rv_bne(m->c, rs1, rs2, 0); rv_ref(m, id, s, 0); }

void rv_mc_ret(struct rv_mc *m) { rv_jalr(m->c, 0, 1, 0); }          /* jalr x0, ra, 0 */
void rv_mc_jr(struct rv_mc *m, int rs) { rv_jalr(m->c, 0, rs, 0); }
void rv_mc_jalr(struct rv_mc *m, int rs) { rv_jalr(m->c, 1, rs, 0); } /* jalr ra, rs, 0 */

/* Continuation re-entry address: needs a code-relative fixup the byte sink does
   not build (the delimited-continuation opcodes are scheme-only and never reach
   the C or Excelsior JIT).  Report it unsupported. */
void rv_mc_lea_label(struct rv_mc *m, int rd, int id)
{ (void)rd; (void)id; m->errsym = "continuation label address unsupported in JIT"; }

/****************************************************************
 * Moves / immediates / addresses
 ****************************************************************/

void rv_mc_li(struct rv_mc *m, int rd, long imm) { li32(m->c, rd, (uint32_t)imm); }
void rv_mc_mv(struct rv_mc *m, int rd, int rs) { rv_addi(m->c, rd, rs, 0); }

void
rv_mc_la(struct rv_mc *m, int rd, const char *name)
{
    uint8_t *ga = kp_global_addr(m->e->j, name);
    if (ga) {
        li32(m->c, rd, (uint32_t)(uintptr_t)ga);
    } else if (kp_is_module_func(m->e->prog, name)) {
        /* the code base is not known yet: emit a fixed lui/addi pair and record a
           fixup patched once the code is mapped */
        size_t site = m->c->len;
        rv_lui(m->c, rd, 0);
        rv_addi(m->c, rd, rd, 0);
        kp_add_absreloc(m->e, site, name);
    } else {
        void *ia = kp_import_addr(m->e, name);
        if (ia)
            li32(m->c, rd, (uint32_t)(uintptr_t)ia);
        else
            m->errsym = name;
    }
}

void
rv_mc_addbig(struct rv_mc *m, int rd, int base, int imm)
{
    if (imm >= -2048 && imm <= 2047) {
        rv_addi(m->c, rd, base, imm);
        return;
    }
    li32(m->c, T6, (uint32_t)imm);
    rv_add(m->c, rd, base, T6);
}

void rv_mc_addi(struct rv_mc *m, int rd, int rs, int imm) { rv_addi(m->c, rd, rs, imm); }
void rv_mc_andi(struct rv_mc *m, int rd, int rs, int imm) { rv_andi(m->c, rd, rs, imm); }
void rv_mc_xori(struct rv_mc *m, int rd, int rs, int imm) { rv_xori(m->c, rd, rs, imm); }
void rv_mc_srai(struct rv_mc *m, int rd, int rs, int shamt) { rv_srai(m->c, rd, rs, shamt); }

/****************************************************************
 * Integer arithmetic and logic
 ****************************************************************/

void rv_mc_add(struct rv_mc *m, int rd, int rs1, int rs2) { rv_add(m->c, rd, rs1, rs2); }
void rv_mc_sub(struct rv_mc *m, int rd, int rs1, int rs2) { rv_sub(m->c, rd, rs1, rs2); }
void rv_mc_mul(struct rv_mc *m, int rd, int rs1, int rs2) { rv_mul(m->c, rd, rs1, rs2); }
void rv_mc_and(struct rv_mc *m, int rd, int rs1, int rs2) { rv_and(m->c, rd, rs1, rs2); }
void rv_mc_or(struct rv_mc *m, int rd, int rs1, int rs2) { rv_or(m->c, rd, rs1, rs2); }
void rv_mc_xor(struct rv_mc *m, int rd, int rs1, int rs2) { rv_xor(m->c, rd, rs1, rs2); }
void rv_mc_sll(struct rv_mc *m, int rd, int rs1, int rs2) { rv_sll(m->c, rd, rs1, rs2); }
void rv_mc_sra(struct rv_mc *m, int rd, int rs1, int rs2) { rv_sra(m->c, rd, rs1, rs2); }
void rv_mc_srl(struct rv_mc *m, int rd, int rs1, int rs2) { rv_srl(m->c, rd, rs1, rs2); }
void rv_mc_div(struct rv_mc *m, int rd, int rs1, int rs2) { rv_div(m->c, rd, rs1, rs2); }
void rv_mc_divu(struct rv_mc *m, int rd, int rs1, int rs2) { rv_divu(m->c, rd, rs1, rs2); }
void rv_mc_rem(struct rv_mc *m, int rd, int rs1, int rs2) { rv_rem(m->c, rd, rs1, rs2); }
void rv_mc_remu(struct rv_mc *m, int rd, int rs1, int rs2) { rv_remu(m->c, rd, rs1, rs2); }
void rv_mc_slt(struct rv_mc *m, int rd, int rs1, int rs2) { rv_slt(m->c, rd, rs1, rs2); }
void rv_mc_sltu(struct rv_mc *m, int rd, int rs1, int rs2) { rv_sltu(m->c, rd, rs1, rs2); }

void rv_mc_neg(struct rv_mc *m, int rd, int rs) { rv_sub(m->c, rd, 0, rs); }        /* neg = sub rd, x0, rs */
void rv_mc_not(struct rv_mc *m, int rd, int rs) { rv_xori(m->c, rd, rs, -1); }      /* not = xori rd, rs, -1 */
void rv_mc_seqz(struct rv_mc *m, int rd, int rs) { rv_sltiu(m->c, rd, rs, 1); }     /* seqz = sltiu rd, rs, 1 */
void rv_mc_snez(struct rv_mc *m, int rd, int rs) { rv_sltu(m->c, rd, 0, rs); }      /* snez = sltu rd, x0, rs */

/****************************************************************
 * Loads and stores, with a scratch-base fallback for a large offset
 ****************************************************************/

typedef void (*rv_ls)(struct code *, int, int, int);

static void
memop(struct rv_mc *m, rv_ls op, int reg, int base, int off)
{
    if (off >= -2048 && off <= 2047) {
        op(m->c, reg, base, off);
    } else {
        li32(m->c, T6, (uint32_t)off);
        rv_add(m->c, T6, base, T6);
        op(m->c, reg, T6, 0);
    }
}

void rv_mc_lb(struct rv_mc *m, int rd, int base, int off) { memop(m, rv_lb, rd, base, off); }
void rv_mc_lh(struct rv_mc *m, int rd, int base, int off) { memop(m, rv_lh, rd, base, off); }
void rv_mc_lw(struct rv_mc *m, int rd, int base, int off) { memop(m, rv_lw, rd, base, off); }
void rv_mc_lbu(struct rv_mc *m, int rd, int base, int off) { memop(m, rv_lbu, rd, base, off); }
void rv_mc_lhu(struct rv_mc *m, int rd, int base, int off) { memop(m, rv_lhu, rd, base, off); }
void rv_mc_sb(struct rv_mc *m, int rs, int base, int off) { memop(m, rv_sb, rs, base, off); }
void rv_mc_sh(struct rv_mc *m, int rs, int base, int off) { memop(m, rv_sh, rs, base, off); }
void rv_mc_sw(struct rv_mc *m, int rs, int base, int off) { memop(m, rv_sw, rs, base, off); }

void rv_mc_flw(struct rv_mc *m, int frd, int base, int off) { memop(m, rv_flw, frd, base, off); }
void rv_mc_fld(struct rv_mc *m, int frd, int base, int off) { memop(m, rv_fld, frd, base, off); }
void rv_mc_flh(struct rv_mc *m, int frd, int base, int off) { memop(m, rv_flh, frd, base, off); }
void rv_mc_fsw(struct rv_mc *m, int frs, int base, int off) { memop(m, rv_fsw, frs, base, off); }
void rv_mc_fsd(struct rv_mc *m, int frs, int base, int off) { memop(m, rv_fsd, frs, base, off); }
void rv_mc_fsh(struct rv_mc *m, int frs, int base, int off) { memop(m, rv_fsh, frs, base, off); }

/****************************************************************
 * Float data processing
 ****************************************************************/

void rv_mc_fadd(struct rv_mc *m, int f32, int fd, int fs1, int fs2)
{ if (f32) rv_fadd_s(m->c, fd, fs1, fs2); else rv_fadd_d(m->c, fd, fs1, fs2); }
void rv_mc_fsub(struct rv_mc *m, int f32, int fd, int fs1, int fs2)
{ if (f32) rv_fsub_s(m->c, fd, fs1, fs2); else rv_fsub_d(m->c, fd, fs1, fs2); }
void rv_mc_fmul(struct rv_mc *m, int f32, int fd, int fs1, int fs2)
{ if (f32) rv_fmul_s(m->c, fd, fs1, fs2); else rv_fmul_d(m->c, fd, fs1, fs2); }
void rv_mc_fdiv(struct rv_mc *m, int f32, int fd, int fs1, int fs2)
{ if (f32) rv_fdiv_s(m->c, fd, fs1, fs2); else rv_fdiv_d(m->c, fd, fs1, fs2); }

void rv_mc_fneg(struct rv_mc *m, int f32, int fd, int fs)
{ if (f32) rv_fsgnjn_s(m->c, fd, fs, fs); else rv_fsgnjn_d(m->c, fd, fs, fs); }
void rv_mc_fabs(struct rv_mc *m, int f32, int fd, int fs)
{ if (f32) rv_fsgnjx_s(m->c, fd, fs, fs); else rv_fsgnjx_d(m->c, fd, fs, fs); }

void rv_mc_feq(struct rv_mc *m, int f32, int rd, int fs1, int fs2)
{ if (f32) rv_feq_s(m->c, rd, fs1, fs2); else rv_feq_d(m->c, rd, fs1, fs2); }
void rv_mc_flt(struct rv_mc *m, int f32, int rd, int fs1, int fs2)
{ if (f32) rv_flt_s(m->c, rd, fs1, fs2); else rv_flt_d(m->c, rd, fs1, fs2); }
void rv_mc_fle(struct rv_mc *m, int f32, int rd, int fs1, int fs2)
{ if (f32) rv_fle_s(m->c, rd, fs1, fs2); else rv_fle_d(m->c, rd, fs1, fs2); }

void rv_mc_fcvt_f_w(struct rv_mc *m, int f32, int fd, int rs)
{ if (f32) rv_fcvt_s_w(m->c, fd, rs, RV_RM_DYN); else rv_fcvt_d_w(m->c, fd, rs, RV_RM_RNE); }
void rv_mc_fcvt_w_f(struct rv_mc *m, int f32, int rd, int fs)
{ if (f32) rv_fcvt_w_s(m->c, rd, fs, RV_RM_RTZ); else rv_fcvt_w_d(m->c, rd, fs, RV_RM_RTZ); }
void rv_mc_fcvt_d_s(struct rv_mc *m, int fd, int fs) { rv_fcvt_d_s(m->c, fd, fs, RV_RM_RNE); }
void rv_mc_fcvt_s_d(struct rv_mc *m, int fd, int fs) { rv_fcvt_s_d(m->c, fd, fs, RV_RM_DYN); }
void rv_mc_fcvt_d_h(struct rv_mc *m, int fd, int fs) { rv_fcvt_d_h(m->c, fd, fs, RV_RM_RNE); }
void rv_mc_fcvt_h_d(struct rv_mc *m, int fd, int fs) { rv_fcvt_h_d(m->c, fd, fs, RV_RM_DYN); }
void rv_mc_fmv_x_w(struct rv_mc *m, int rd, int fs) { rv_fmv_x_w(m->c, rd, fs); }
void rv_mc_fmv_w_x(struct rv_mc *m, int fd, int rs) { rv_fmv_w_x(m->c, fd, rs); }

/****************************************************************
 * Calls
 *
 * A module function is a direct jal with a deferred relocation; a runtime symbol
 * is a host binding whose address is materialised in t6 and called through.
 ****************************************************************/

void
rv_mc_call(struct rv_mc *m, const char *name)
{
    if (kp_is_module_func(m->e->prog, name)) {
        size_t site = m->c->len;
        rv_jal(m->c, 1, 0);             /* jal ra, <patched> */
        kp_add_reloc(m->e, site, name);
        return;
    }
    void *a = kp_import_addr(m->e, name);
    if (!a) { m->errsym = name; return; }
    li32(m->c, T6, (uint32_t)(uintptr_t)a);
    rv_jalr(m->c, 1, T6, 0);            /* jalr ra, t6, 0 */
}

void
rv_mc_j_sym(struct rv_mc *m, const char *name)
{
    if (kp_is_module_func(m->e->prog, name)) {
        size_t site = m->c->len;
        rv_jal(m->c, 0, 0);             /* j <patched> (jal x0) */
        kp_add_reloc(m->e, site, name);
        return;
    }
    void *a = kp_import_addr(m->e, name);
    if (!a) { m->errsym = name; return; }
    li32(m->c, T6, (uint32_t)(uintptr_t)a);
    rv_jalr(m->c, 0, T6, 0);            /* jr t6 */
}

/* Inline assembly has no byte-sink form: report it unsupported (the cc JIT suite
   excludes the inline-asm test, so this is never reached). */
void rv_mc_asm(struct rv_mc *m, const char *text) { m->errsym = text; }

/****************************************************************
 * kp_target: the RISC-V back end kp_jit() drives
 ****************************************************************/

static int
emit_func(struct emitctx *e, struct code *c, struct ir_func *fn)
{
    struct rv_mc M;
    char errbuf[128];
    int rc;

    rv_reset(&M, c, e);
    rc = rv_select_func(&M, fn, errbuf, sizeof errbuf);
    if (rc == 0 && M.errsym)
        rc = kp_fail(e, "unresolved symbol '%s'", M.errsym, 0);
    else if (rc != 0)
        rc = kp_fail(e, "%s", errbuf, 0);
    rv_free_sink(&M);
    return rc;
}

/* A deferred intra-module call is a J-type jal whose 20-bit word offset is
   patched; -1 if the target is beyond the +/-1MB range. */
static int
rv_apply_call_reloc(struct code *c, size_t site, size_t target)
{
    int32_t disp = (int32_t)((long)target - (long)site);
    return rv_patch_j(c, site, disp);
}

/* An absolute function-code reference is the lui/addi pair rv_mc_la emitted:
   split the address the same way li32 does and patch both immediates. */
static void
rv_apply_abs_reloc(uint8_t *code, size_t site, uint64_t addr)
{
    uint32_t v = (uint32_t)addr;
    int32_t val = (int32_t)v;
    int32_t hi = (val + 0x800) >> 12;
    int32_t lo = val - (hi << 12);
    uint32_t w0, w1;

    memcpy(&w0, code + site, 4);
    memcpy(&w1, code + site + 4, 4);
    w0 = (w0 & 0x00000fffu) | (((uint32_t)hi & 0xfffffu) << 12);   /* lui imm[31:12] */
    w1 = (w1 & 0x000fffffu) | (((uint32_t)lo & 0xfffu) << 20);     /* addi imm[11:0] */
    memcpy(code + site, &w0, 4);
    memcpy(code + site + 4, &w1, 4);
}

const struct jit_target kp_target = {
    emit_func,
    rv_apply_call_reloc,
    rv_apply_abs_reloc,
};

/****************************************************************
 * Running a compiled entry
 ****************************************************************/

void
kp_jit_call(struct kp_jit *j, void *entry)
{
    uintptr_t top = ((uintptr_t)j->stack + j->stack_len) & ~(uintptr_t)15;

    /* Save the host sp in a callee-saved register (entry preserves s0 per the
       psABI), switch to the low stack for the call, then restore.  entry takes
       no arguments and returns nothing here. */
    __asm__ volatile (
        "mv s0, sp\n\t"
        "mv sp, %0\n\t"
        "jalr ra, %1, 0\n\t"
        "mv sp, s0\n\t"
        :
        : "r"(top), "r"(entry)
        : "s0", "ra", "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7",
          "t0", "t1", "t2", "t3", "t4", "t5", "t6", "memory");
}
