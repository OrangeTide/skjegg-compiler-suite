/* emit_rv.h : RISC-V RV32 machine-code encoder (byte emitter).

   The byte-level counterpart of backend/rv_mc_text.c, which emits GAS text for
   the AOT path (both driven by the shared selector backend/rv_select.c).  This
   encoder writes raw 32-bit instruction words into a growable
   buffer, the form an in-process JIT needs.  The opcode, funct3, and funct7
   values mirror as/rv_encode.c (the skj-as-rv assembler), and every encoder
   here is checked byte for byte against riscv64-linux-gnu-as by
   tests/rv_oracle.c.

   Operands are hardware register numbers 0..31 (x0..x31 for integer,
   f0..f31 for float).  The RV32 psABI names the JIT glue uses are below. */

#ifndef EMIT_RV_H
#define EMIT_RV_H

#include <stddef.h>
#include <stdint.h>

#include "code.h"

/* Integer register numbers (RV32 psABI names). */
enum {
    RV_ZERO = 0, RV_RA = 1, RV_SP = 2, RV_GP = 3, RV_TP = 4,
    RV_T0 = 5, RV_T1 = 6, RV_T2 = 7, RV_FP = 8, RV_S1 = 9,
    RV_A0 = 10, RV_A1 = 11, RV_A2 = 12, RV_A3 = 13, RV_A4 = 14,
    RV_A5 = 15, RV_A6 = 16, RV_A7 = 17,
    RV_S2 = 18, RV_S3 = 19, RV_S4 = 20, RV_S5 = 21, RV_S6 = 22,
    RV_S7 = 23, RV_S8 = 24, RV_S9 = 25, RV_S10 = 26, RV_S11 = 27,
    RV_T3 = 28, RV_T4 = 29, RV_T5 = 30, RV_T6 = 31,
};

/* The floating-point rounding-mode field (funct3 of an FP op): dyn follows the
   fcsr, the others force a mode.  The backend, like GAS, defaults to dyn. */
enum { RV_RM_RNE = 0, RV_RM_RTZ = 1, RV_RM_DYN = 7 };

/* ---- the six base instruction formats (pure field packers) ---- */
uint32_t rv_enc_r(int op, int f3, int f7, int rd, int rs1, int rs2);
uint32_t rv_enc_i(int op, int f3, int rd, int rs1, int imm);
uint32_t rv_enc_s(int op, int f3, int rs1, int rs2, int imm);
uint32_t rv_enc_b(int op, int f3, int rs1, int rs2, int imm);
uint32_t rv_enc_u(int op, int rd, int imm20);
uint32_t rv_enc_j(int op, int rd, int imm);

/* ---- RV32I / M integer ops (rd, rs1, rs2) ---- */
void rv_add(struct code *c, int rd, int rs1, int rs2);
void rv_sub(struct code *c, int rd, int rs1, int rs2);
void rv_sll(struct code *c, int rd, int rs1, int rs2);
void rv_slt(struct code *c, int rd, int rs1, int rs2);
void rv_sltu(struct code *c, int rd, int rs1, int rs2);
void rv_xor(struct code *c, int rd, int rs1, int rs2);
void rv_srl(struct code *c, int rd, int rs1, int rs2);
void rv_sra(struct code *c, int rd, int rs1, int rs2);
void rv_or(struct code *c, int rd, int rs1, int rs2);
void rv_and(struct code *c, int rd, int rs1, int rs2);
void rv_mul(struct code *c, int rd, int rs1, int rs2);
void rv_mulh(struct code *c, int rd, int rs1, int rs2);
void rv_mulhu(struct code *c, int rd, int rs1, int rs2);
void rv_mulhsu(struct code *c, int rd, int rs1, int rs2);
void rv_div(struct code *c, int rd, int rs1, int rs2);
void rv_divu(struct code *c, int rd, int rs1, int rs2);
void rv_rem(struct code *c, int rd, int rs1, int rs2);
void rv_remu(struct code *c, int rd, int rs1, int rs2);

/* ---- OP-IMM (rd, rs1, imm12); shifts take a shamt 0..31 ---- */
void rv_addi(struct code *c, int rd, int rs1, int imm);
void rv_slti(struct code *c, int rd, int rs1, int imm);
void rv_sltiu(struct code *c, int rd, int rs1, int imm);
void rv_xori(struct code *c, int rd, int rs1, int imm);
void rv_ori(struct code *c, int rd, int rs1, int imm);
void rv_andi(struct code *c, int rd, int rs1, int imm);
void rv_slli(struct code *c, int rd, int rs1, int shamt);
void rv_srli(struct code *c, int rd, int rs1, int shamt);
void rv_srai(struct code *c, int rd, int rs1, int shamt);

/* ---- loads (rd, imm(rs1)) and stores (rs2, imm(rs1)) ---- */
void rv_lb(struct code *c, int rd, int rs1, int imm);
void rv_lh(struct code *c, int rd, int rs1, int imm);
void rv_lw(struct code *c, int rd, int rs1, int imm);
void rv_lbu(struct code *c, int rd, int rs1, int imm);
void rv_lhu(struct code *c, int rd, int rs1, int imm);
void rv_sb(struct code *c, int rs2, int rs1, int imm);
void rv_sh(struct code *c, int rs2, int rs1, int imm);
void rv_sw(struct code *c, int rs2, int rs1, int imm);

/* ---- U-type and jumps ---- */
void rv_lui(struct code *c, int rd, int imm20);
void rv_auipc(struct code *c, int rd, int imm20);
void rv_jal(struct code *c, int rd, int imm);
void rv_jalr(struct code *c, int rd, int rs1, int imm);

/* ---- B-type conditional branches (rs1, rs2, byte displacement) ---- */
void rv_beq(struct code *c, int rs1, int rs2, int imm);
void rv_bne(struct code *c, int rs1, int rs2, int imm);
void rv_blt(struct code *c, int rs1, int rs2, int imm);
void rv_bge(struct code *c, int rs1, int rs2, int imm);
void rv_bltu(struct code *c, int rs1, int rs2, int imm);
void rv_bgeu(struct code *c, int rs1, int rs2, int imm);

/* ---- float loads/stores (float reg, imm(int base)) ---- */
void rv_flw(struct code *c, int frd, int rs1, int imm);
void rv_fld(struct code *c, int frd, int rs1, int imm);
void rv_flh(struct code *c, int frd, int rs1, int imm);
void rv_fsw(struct code *c, int frs, int rs1, int imm);
void rv_fsd(struct code *c, int frs, int rs1, int imm);
void rv_fsh(struct code *c, int frs, int rs1, int imm);

/* ---- float arithmetic (three float regs), rm = dyn ---- */
void rv_fadd_s(struct code *c, int fd, int fs1, int fs2);
void rv_fsub_s(struct code *c, int fd, int fs1, int fs2);
void rv_fmul_s(struct code *c, int fd, int fs1, int fs2);
void rv_fdiv_s(struct code *c, int fd, int fs1, int fs2);
void rv_fadd_d(struct code *c, int fd, int fs1, int fs2);
void rv_fsub_d(struct code *c, int fd, int fs1, int fs2);
void rv_fmul_d(struct code *c, int fd, int fs1, int fs2);
void rv_fdiv_d(struct code *c, int fd, int fs1, int fs2);
void rv_fadd_h(struct code *c, int fd, int fs1, int fs2);
void rv_fsub_h(struct code *c, int fd, int fs1, int fs2);
void rv_fmul_h(struct code *c, int fd, int fs1, int fs2);
void rv_fdiv_h(struct code *c, int fd, int fs1, int fs2);

/* ---- sign-inject (used to synthesize fmv/fneg/fabs) and min/max ---- */
void rv_fsgnj_s(struct code *c, int fd, int fs1, int fs2);
void rv_fsgnjn_s(struct code *c, int fd, int fs1, int fs2);
void rv_fsgnjx_s(struct code *c, int fd, int fs1, int fs2);
void rv_fsgnj_d(struct code *c, int fd, int fs1, int fs2);
void rv_fsgnjn_d(struct code *c, int fd, int fs1, int fs2);
void rv_fsgnjx_d(struct code *c, int fd, int fs1, int fs2);
void rv_fmin_s(struct code *c, int fd, int fs1, int fs2);
void rv_fmax_s(struct code *c, int fd, int fs1, int fs2);
void rv_fmin_d(struct code *c, int fd, int fs1, int fs2);
void rv_fmax_d(struct code *c, int fd, int fs1, int fs2);

/* ---- float compares (int rd, two float regs) ---- */
void rv_feq_s(struct code *c, int rd, int fs1, int fs2);
void rv_flt_s(struct code *c, int rd, int fs1, int fs2);
void rv_fle_s(struct code *c, int rd, int fs1, int fs2);
void rv_feq_d(struct code *c, int rd, int fs1, int fs2);
void rv_flt_d(struct code *c, int rd, int fs1, int fs2);
void rv_fle_d(struct code *c, int rd, int fs1, int fs2);

/* ---- fsqrt (two float regs), rm = dyn ---- */
void rv_fsqrt_s(struct code *c, int fd, int fs1);
void rv_fsqrt_d(struct code *c, int fd, int fs1);

/* ---- moves between integer and float registers (bit pattern) ---- */
void rv_fmv_x_w(struct code *c, int rd, int fs1);   /* int <- float bits */
void rv_fmv_w_x(struct code *c, int fd, int rs1);   /* float <- int bits */
void rv_fmv_x_h(struct code *c, int rd, int fs1);
void rv_fmv_h_x(struct code *c, int fd, int rs1);

/* ---- conversions.  rm is a RV_RM_* code (the backend uses dyn for a rounding
   conversion and rne for an exact one; the encoder takes it explicitly). ---- */
void rv_fcvt_s_w(struct code *c, int fd, int rs1, int rm);
void rv_fcvt_s_wu(struct code *c, int fd, int rs1, int rm);
void rv_fcvt_d_w(struct code *c, int fd, int rs1, int rm);
void rv_fcvt_d_wu(struct code *c, int fd, int rs1, int rm);
void rv_fcvt_h_w(struct code *c, int fd, int rs1, int rm);
void rv_fcvt_h_wu(struct code *c, int fd, int rs1, int rm);
void rv_fcvt_w_s(struct code *c, int rd, int fs1, int rm);
void rv_fcvt_wu_s(struct code *c, int rd, int fs1, int rm);
void rv_fcvt_w_d(struct code *c, int rd, int fs1, int rm);
void rv_fcvt_wu_d(struct code *c, int rd, int fs1, int rm);
void rv_fcvt_w_h(struct code *c, int rd, int fs1, int rm);
void rv_fcvt_wu_h(struct code *c, int rd, int fs1, int rm);
void rv_fcvt_s_d(struct code *c, int fd, int fs1, int rm);
void rv_fcvt_d_s(struct code *c, int fd, int fs1, int rm);
void rv_fcvt_s_h(struct code *c, int fd, int fs1, int rm);
void rv_fcvt_h_s(struct code *c, int fd, int fs1, int rm);
void rv_fcvt_d_h(struct code *c, int fd, int fs1, int rm);
void rv_fcvt_h_d(struct code *c, int fd, int fs1, int rm);

/* Patch a B-type branch (at byte offset `site` in c->buf) to reach `target`;
   returns -1 if the displacement is out of the +/-4KB range. */
int rv_patch_b(struct code *c, size_t site, int32_t disp);
/* Patch a J-type jal (at `site`) to reach `target`; -1 if beyond +/-1MB. */
int rv_patch_j(struct code *c, size_t site, int32_t disp);

#endif /* EMIT_RV_H */
