/* emit_arm64.h : AArch64 (ARM64) machine-code encoder (byte emitter).

   The byte-level counterpart of backend/arm64_emit.c, which emits GAS text for
   the AOT path.  This encoder writes raw 32-bit instruction words into a
   growable buffer, the form an in-process JIT needs.  Every encoder here is
   checked byte for byte against aarch64-linux-gnu-as by tests/arm64_oracle.c.

   Operands are hardware register numbers 0..31.  For most instructions 31 is
   the zero register (xzr/wzr); for add/sub-immediate and load/store base it is
   the stack pointer (sp).  A 32-bit operation (sf == 0) writes the w-view and
   zeroes the upper half; sf == 1 is the full 64-bit x-view. */

#ifndef EMIT_ARM64_H
#define EMIT_ARM64_H

#include <stddef.h>
#include <stdint.h>

#include "code.h"

/* Named registers used by the JIT glue. */
enum {
    A_FP = 29,      /* frame pointer (x29) */
    A_LR = 30,      /* link register (x30) */
    A_SP = 31,      /* stack pointer (in the add/sub-imm and ldst base slot) */
    A_ZR = 31,      /* zero register (elsewhere) */
};

/* Condition codes (the 4-bit field of B.cond / CSINC). */
enum a_cc {
    A_EQ = 0x0, A_NE = 0x1, A_CS = 0x2, A_CC = 0x3,
    A_MI = 0x4, A_PL = 0x5, A_VS = 0x6, A_VC = 0x7,
    A_HI = 0x8, A_LS = 0x9, A_GE = 0xA, A_LT = 0xB,
    A_GT = 0xC, A_LE = 0xD, A_AL = 0xE,
};

/* ---- Move wide immediate.  shift is 0/16/32/48. */
void a_movz(struct code *c, int sf, int Rd, unsigned imm16, int shift);
void a_movk(struct code *c, int sf, int Rd, unsigned imm16, int shift);
void a_movn(struct code *c, int sf, int Rd, unsigned imm16, int shift);
/* Materialize an arbitrary value into Rd with a movz/movk chain (the fewest
   16-bit lanes needed).  sf selects the 32- or 64-bit destination. */
void a_movimm(struct code *c, int sf, int Rd, uint64_t v);

/* ---- Add/subtract immediate (12-bit, optional lsl #12).  Rd/Rn may be sp. */
void a_add_imm(struct code *c, int sf, int Rd, int Rn, unsigned imm12, int lsl12);
void a_sub_imm(struct code *c, int sf, int Rd, int Rn, unsigned imm12, int lsl12);
/* Rd = Rn + imm for any signed imm (64-bit), materialising through `scratch`
   when it does not fit an add/sub-immediate form.  The counterpart of the AOT
   backend's addimm helper. */
void a_addimm(struct code *c, int Rd, int Rn, long imm, int scratch);

/* ---- Data-processing, three-register (shifted-register form, shift 0). */
void a_add_reg(struct code *c, int sf, int Rd, int Rn, int Rm);
void a_sub_reg(struct code *c, int sf, int Rd, int Rn, int Rm);
void a_and_reg(struct code *c, int sf, int Rd, int Rn, int Rm);
void a_orr_reg(struct code *c, int sf, int Rd, int Rn, int Rm);
void a_eor_reg(struct code *c, int sf, int Rd, int Rn, int Rm);
void a_mul(struct code *c, int sf, int Rd, int Rn, int Rm);
void a_msub(struct code *c, int sf, int Rd, int Rn, int Rm, int Ra);
void a_sdiv(struct code *c, int sf, int Rd, int Rn, int Rm);
void a_udiv(struct code *c, int sf, int Rd, int Rn, int Rm);
void a_lslv(struct code *c, int sf, int Rd, int Rn, int Rm);   /* variable shift by Rm */
void a_asrv(struct code *c, int sf, int Rd, int Rn, int Rm);
void a_lsrv(struct code *c, int sf, int Rd, int Rn, int Rm);

/* ---- Data-processing, two-register / moves. */
void a_neg(struct code *c, int sf, int Rd, int Rm);   /* sub Rd, zr, Rm */
void a_mvn(struct code *c, int sf, int Rd, int Rm);   /* orn Rd, zr, Rm */
void a_mov_reg(struct code *c, int sf, int Rd, int Rn); /* orr Rd, zr, Rn */
void a_sxtw(struct code *c, int Rd, int Rn);          /* sign-extend Wn -> Xd */

/* ---- Compare and condition materialization. */
void a_cmp_reg(struct code *c, int sf, int Rn, int Rm); /* subs zr, Rn, Rm */
void a_cset(struct code *c, int sf, int Rd, int cc);    /* csinc Rd, zr, zr, !cc */

/* ---- Integer loads and stores, base+offset.  The encoder selects the scaled
   unsigned-offset form when the offset fits, else the unscaled signed 9-bit
   form; a larger offset must be materialised by the caller (through a scratch
   base).  A sign-extending load writes the 32-bit (Wt) result. */
void a_strb(struct code *c, int Rt, int Rn, int off);
void a_strh(struct code *c, int Rt, int Rn, int off);
void a_strw(struct code *c, int Rt, int Rn, int off);
void a_strx(struct code *c, int Rt, int Rn, int off);
void a_ldrb(struct code *c, int Rt, int Rn, int off);
void a_ldrsb(struct code *c, int Rt, int Rn, int off);   /* -> Wt */
void a_ldrh(struct code *c, int Rt, int Rn, int off);
void a_ldrsh(struct code *c, int Rt, int Rn, int off);   /* -> Wt */
void a_ldrw(struct code *c, int Rt, int Rn, int off);
void a_ldrx(struct code *c, int Rt, int Rn, int off);

/* ---- FP/SIMD scalar loads and stores.  sz: 1 = half (h), 2 = single (s),
   3 = double (d). */
void a_str_fp(struct code *c, int sz, int Rt, int Rn, int off);
void a_ldr_fp(struct code *c, int sz, int Rt, int Rn, int off);

/* ---- FP data processing.  sz: 2 = single, 3 = double (1 = half where valid). */
void a_fmov(struct code *c, int sz, int Rd, int Rn);
void a_fadd(struct code *c, int sz, int Rd, int Rn, int Rm);
void a_fsub(struct code *c, int sz, int Rd, int Rn, int Rm);
void a_fmul(struct code *c, int sz, int Rd, int Rn, int Rm);
void a_fdiv(struct code *c, int sz, int Rd, int Rn, int Rm);
void a_fneg(struct code *c, int sz, int Rd, int Rn);
void a_fabs(struct code *c, int sz, int Rd, int Rn);
void a_fsqrt(struct code *c, int sz, int Rd, int Rn);
void a_fcmp(struct code *c, int sz, int Rn, int Rm);
void a_fcvt(struct code *c, int dstsz, int srcsz, int Rd, int Rn);  /* between H/S/D */
void a_scvtf(struct code *c, int sz, int Rd, int Rn);   /* Wn (int) -> Sd/Dd */
void a_fcvtzs(struct code *c, int sz, int Rd, int Rn);  /* Sn/Dn -> Wd, toward zero */
void a_fcvtns(struct code *c, int sz, int Rd, int Rn);  /* Sn/Dn -> Wd, to nearest */

/* ---- Control flow.  A branch to a label is emitted with a placeholder offset;
   the sink patches the site later with a_patch_imm26 (b/bl) or a_patch_imm19
   (b.cond/cbz/cbnz).  Each emitter returns the byte offset of its instruction. */
size_t a_b(struct code *c);
size_t a_bl(struct code *c);
size_t a_bcond(struct code *c, int cc);
size_t a_cbz(struct code *c, int sf, int Rt);
size_t a_cbnz(struct code *c, int sf, int Rt);
/* Patch a branch's immediate to reach byte offset target.  Returns 0, or -1 if
   the word-scaled displacement does not fit the field (imm26: +/-128MB; imm19:
   +/-1MB), so the caller can reject the program rather than emit a wrong jump. */
int a_patch_imm26(struct code *c, size_t site, size_t target);
int a_patch_imm19(struct code *c, size_t site, size_t target);
void a_blr(struct code *c, int Rn);
void a_br(struct code *c, int Rn);
void a_ret(struct code *c);       /* ret x30 */
void a_nop(struct code *c);

#endif /* EMIT_ARM64_H */
