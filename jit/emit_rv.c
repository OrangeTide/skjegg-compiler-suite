/* emit_rv.c : the RV32 machine-code encoder (see emit_rv.h).

   The field packers and per-instruction opcode/funct values mirror
   as/rv_encode.c, the skj-as-rv assembler that is itself checked against GNU as;
   tests/rv_oracle.c re-checks this encoder the same way. */

#include "emit_rv.h"

/* ---- the six base formats ---- */

uint32_t
rv_enc_r(int op, int f3, int f7, int rd, int rs1, int rs2)
{
    return (uint32_t)op | ((uint32_t)rd << 7) | ((uint32_t)f3 << 12) |
           ((uint32_t)rs1 << 15) | ((uint32_t)rs2 << 20) | ((uint32_t)f7 << 25);
}

uint32_t
rv_enc_i(int op, int f3, int rd, int rs1, int imm)
{
    return (uint32_t)op | ((uint32_t)rd << 7) | ((uint32_t)f3 << 12) |
           ((uint32_t)rs1 << 15) | (((uint32_t)imm & 0xfff) << 20);
}

uint32_t
rv_enc_s(int op, int f3, int rs1, int rs2, int imm)
{
    return (uint32_t)op | (((uint32_t)imm & 0x1f) << 7) | ((uint32_t)f3 << 12) |
           ((uint32_t)rs1 << 15) | ((uint32_t)rs2 << 20) |
           ((((uint32_t)imm >> 5) & 0x7f) << 25);
}

uint32_t
rv_enc_b(int op, int f3, int rs1, int rs2, int imm)
{
    return (uint32_t)op |
           ((((uint32_t)imm >> 11) & 1) << 7) |
           ((((uint32_t)imm >> 1) & 0xf) << 8) |
           ((uint32_t)f3 << 12) | ((uint32_t)rs1 << 15) | ((uint32_t)rs2 << 20) |
           ((((uint32_t)imm >> 5) & 0x3f) << 25) |
           ((((uint32_t)imm >> 12) & 1) << 31);
}

uint32_t
rv_enc_u(int op, int rd, int imm20)
{
    return (uint32_t)op | ((uint32_t)rd << 7) |
           (((uint32_t)imm20 & 0xfffff) << 12);
}

uint32_t
rv_enc_j(int op, int rd, int imm)
{
    return (uint32_t)op | ((uint32_t)rd << 7) |
           ((((uint32_t)imm >> 12) & 0xff) << 12) |
           ((((uint32_t)imm >> 11) & 1) << 20) |
           ((((uint32_t)imm >> 1) & 0x3ff) << 21) |
           ((((uint32_t)imm >> 20) & 1) << 31);
}

/* ---- RV32I / M integer register ops (op 0x33) ---- */
#define R(name, f3, f7) \
    void name(struct code *c, int rd, int rs1, int rs2) \
    { emit32(c, rv_enc_r(0x33, (f3), (f7), rd, rs1, rs2)); }

R(rv_add, 0, 0x00)  R(rv_sub, 0, 0x20)  R(rv_sll, 1, 0x00)
R(rv_slt, 2, 0x00)  R(rv_sltu, 3, 0x00) R(rv_xor, 4, 0x00)
R(rv_srl, 5, 0x00)  R(rv_sra, 5, 0x20)  R(rv_or, 6, 0x00)
R(rv_and, 7, 0x00)
R(rv_mul, 0, 0x01)  R(rv_mulh, 1, 0x01) R(rv_mulhsu, 2, 0x01)
R(rv_mulhu, 3, 0x01) R(rv_div, 4, 0x01) R(rv_divu, 5, 0x01)
R(rv_rem, 6, 0x01)  R(rv_remu, 7, 0x01)
#undef R

/* ---- OP-IMM (op 0x13) ---- */
#define I(name, f3) \
    void name(struct code *c, int rd, int rs1, int imm) \
    { emit32(c, rv_enc_i(0x13, (f3), rd, rs1, imm)); }

I(rv_addi, 0)  I(rv_slti, 2)  I(rv_sltiu, 3)
I(rv_xori, 4)  I(rv_ori, 6)   I(rv_andi, 7)
#undef I

/* shift-immediate: the shamt sits in the low 5 bits, funct7 selects the op */
#define SH(name, f3, f7) \
    void name(struct code *c, int rd, int rs1, int shamt) \
    { emit32(c, rv_enc_i(0x13, (f3), rd, rs1, ((f7) << 5) | (shamt & 0x1f))); }

SH(rv_slli, 1, 0x00)  SH(rv_srli, 5, 0x00)  SH(rv_srai, 5, 0x20)
#undef SH

/* ---- loads (op 0x03) and stores (op 0x23) ---- */
#define LD(name, f3) \
    void name(struct code *c, int rd, int rs1, int imm) \
    { emit32(c, rv_enc_i(0x03, (f3), rd, rs1, imm)); }

LD(rv_lb, 0)  LD(rv_lh, 1)  LD(rv_lw, 2)  LD(rv_lbu, 4)  LD(rv_lhu, 5)
#undef LD

#define ST(name, f3) \
    void name(struct code *c, int rs2, int rs1, int imm) \
    { emit32(c, rv_enc_s(0x23, (f3), rs1, rs2, imm)); }

ST(rv_sb, 0)  ST(rv_sh, 1)  ST(rv_sw, 2)
#undef ST

/* ---- U-type and jumps ---- */
void rv_lui(struct code *c, int rd, int imm20)   { emit32(c, rv_enc_u(0x37, rd, imm20)); }
void rv_auipc(struct code *c, int rd, int imm20) { emit32(c, rv_enc_u(0x17, rd, imm20)); }
void rv_jal(struct code *c, int rd, int imm)     { emit32(c, rv_enc_j(0x6f, rd, imm)); }
void rv_jalr(struct code *c, int rd, int rs1, int imm)
{ emit32(c, rv_enc_i(0x67, 0, rd, rs1, imm)); }

/* ---- B-type branches (op 0x63) ---- */
#define B(name, f3) \
    void name(struct code *c, int rs1, int rs2, int imm) \
    { emit32(c, rv_enc_b(0x63, (f3), rs1, rs2, imm)); }

B(rv_beq, 0)  B(rv_bne, 1)  B(rv_blt, 4)  B(rv_bge, 5)
B(rv_bltu, 6) B(rv_bgeu, 7)
#undef B

/* ---- float loads (op 0x07) and stores (op 0x27) ---- */
#define FLD(name, f3) \
    void name(struct code *c, int frd, int rs1, int imm) \
    { emit32(c, rv_enc_i(0x07, (f3), frd, rs1, imm)); }

FLD(rv_flh, 1)  FLD(rv_flw, 2)  FLD(rv_fld, 3)
#undef FLD

#define FST(name, f3) \
    void name(struct code *c, int frs, int rs1, int imm) \
    { emit32(c, rv_enc_s(0x27, (f3), rs1, frs, imm)); }

FST(rv_fsh, 1)  FST(rv_fsw, 2)  FST(rv_fsd, 3)
#undef FST

/* ---- float arithmetic (op 0x53, rm = dyn) ---- */
#define FA(name, f7) \
    void name(struct code *c, int fd, int fs1, int fs2) \
    { emit32(c, rv_enc_r(0x53, RV_RM_DYN, (f7), fd, fs1, fs2)); }

FA(rv_fadd_s, 0x00)  FA(rv_fsub_s, 0x04)  FA(rv_fmul_s, 0x08)  FA(rv_fdiv_s, 0x0c)
FA(rv_fadd_d, 0x01)  FA(rv_fsub_d, 0x05)  FA(rv_fmul_d, 0x09)  FA(rv_fdiv_d, 0x0d)
FA(rv_fadd_h, 0x02)  FA(rv_fsub_h, 0x06)  FA(rv_fmul_h, 0x0a)  FA(rv_fdiv_h, 0x0e)
#undef FA

/* ---- sign-inject and min/max (op 0x53, explicit funct3) ---- */
#define FR(name, f3, f7) \
    void name(struct code *c, int fd, int fs1, int fs2) \
    { emit32(c, rv_enc_r(0x53, (f3), (f7), fd, fs1, fs2)); }

FR(rv_fsgnj_s, 0, 0x10)   FR(rv_fsgnjn_s, 1, 0x10)  FR(rv_fsgnjx_s, 2, 0x10)
FR(rv_fsgnj_d, 0, 0x11)   FR(rv_fsgnjn_d, 1, 0x11)  FR(rv_fsgnjx_d, 2, 0x11)
FR(rv_fmin_s, 0, 0x14)    FR(rv_fmax_s, 1, 0x14)
FR(rv_fmin_d, 0, 0x15)    FR(rv_fmax_d, 1, 0x15)
#undef FR

/* ---- float compares (int rd, op 0x53) ---- */
#define FC(name, f3, f7) \
    void name(struct code *c, int rd, int fs1, int fs2) \
    { emit32(c, rv_enc_r(0x53, (f3), (f7), rd, fs1, fs2)); }

FC(rv_fle_s, 0, 0x50)  FC(rv_flt_s, 1, 0x50)  FC(rv_feq_s, 2, 0x50)
FC(rv_fle_d, 0, 0x51)  FC(rv_flt_d, 1, 0x51)  FC(rv_feq_d, 2, 0x51)
#undef FC

/* ---- fsqrt (rm = dyn, rs2 field 0) ---- */
void rv_fsqrt_s(struct code *c, int fd, int fs1)
{ emit32(c, rv_enc_r(0x53, RV_RM_DYN, 0x2c, fd, fs1, 0)); }
void rv_fsqrt_d(struct code *c, int fd, int fs1)
{ emit32(c, rv_enc_r(0x53, RV_RM_DYN, 0x2d, fd, fs1, 0)); }

/* ---- int/float register moves (funct3 0, rs2 0) ---- */
void rv_fmv_x_w(struct code *c, int rd, int fs1)
{ emit32(c, rv_enc_r(0x53, 0, 0x70, rd, fs1, 0)); }
void rv_fmv_w_x(struct code *c, int fd, int rs1)
{ emit32(c, rv_enc_r(0x53, 0, 0x78, fd, rs1, 0)); }
void rv_fmv_x_h(struct code *c, int rd, int fs1)
{ emit32(c, rv_enc_r(0x53, 0, 0x72, rd, fs1, 0)); }
void rv_fmv_h_x(struct code *c, int fd, int rs1)
{ emit32(c, rv_enc_r(0x53, 0, 0x7a, fd, rs1, 0)); }

/* ---- conversions (op 0x53); rm is the rounding field, rs2 selects source ---- */
#define CV(name, f7, rs2) \
    void name(struct code *c, int rd, int rs1, int rm) \
    { emit32(c, rv_enc_r(0x53, rm, (f7), rd, rs1, (rs2))); }

CV(rv_fcvt_s_w, 0x68, 0)   CV(rv_fcvt_s_wu, 0x68, 1)
CV(rv_fcvt_d_w, 0x69, 0)   CV(rv_fcvt_d_wu, 0x69, 1)
CV(rv_fcvt_h_w, 0x6a, 0)   CV(rv_fcvt_h_wu, 0x6a, 1)
CV(rv_fcvt_w_s, 0x60, 0)   CV(rv_fcvt_wu_s, 0x60, 1)
CV(rv_fcvt_w_d, 0x61, 0)   CV(rv_fcvt_wu_d, 0x61, 1)
CV(rv_fcvt_w_h, 0x62, 0)   CV(rv_fcvt_wu_h, 0x62, 1)
CV(rv_fcvt_s_d, 0x20, 1)   CV(rv_fcvt_d_s, 0x21, 0)
CV(rv_fcvt_s_h, 0x20, 2)   CV(rv_fcvt_h_s, 0x22, 0)
CV(rv_fcvt_d_h, 0x21, 2)   CV(rv_fcvt_h_d, 0x22, 1)
#undef CV

/* ---- branch/jump displacement patching ---- */

#define B_IMM_MASK  ((1u << 7) | (0xfu << 8) | (0x3fu << 25) | (1u << 31))
#define J_IMM_MASK  ((0xffu << 12) | (1u << 20) | (0x3ffu << 21) | (1u << 31))

static uint32_t
rd_word(struct code *c, size_t site)
{
    uint32_t w;
    w = (uint32_t)c->buf[site] | ((uint32_t)c->buf[site + 1] << 8) |
        ((uint32_t)c->buf[site + 2] << 16) | ((uint32_t)c->buf[site + 3] << 24);
    return w;
}

static void
wr_word(struct code *c, size_t site, uint32_t w)
{
    c->buf[site] = (uint8_t)w;
    c->buf[site + 1] = (uint8_t)(w >> 8);
    c->buf[site + 2] = (uint8_t)(w >> 16);
    c->buf[site + 3] = (uint8_t)(w >> 24);
}

int
rv_patch_b(struct code *c, size_t site, int32_t disp)
{
    uint32_t w;
    if (disp < -4096 || disp > 4094 || (disp & 1))
        return -1;
    w = rd_word(c, site);
    w = (w & ~B_IMM_MASK) | (rv_enc_b(0, 0, 0, 0, disp) & B_IMM_MASK);
    wr_word(c, site, w);
    return 0;
}

int
rv_patch_j(struct code *c, size_t site, int32_t disp)
{
    uint32_t w;
    if (disp < -(1 << 20) || disp > (1 << 20) - 2 || (disp & 1))
        return -1;
    w = rd_word(c, site);
    w = (w & ~J_IMM_MASK) | (rv_enc_j(0, 0, disp) & J_IMM_MASK);
    wr_word(c, site, w);
    return 0;
}
