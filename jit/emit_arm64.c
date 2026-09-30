/* emit_arm64.c : AArch64 machine-code encoder (see emit_arm64.h).

   Each instruction is one little-endian 32-bit word.  The encodings follow the
   Arm A-profile manual's fixed field layouts; tests/arm64_oracle.c checks every
   encoder byte for byte against aarch64-linux-gnu-as. */

#include "emit_arm64.h"

#include <string.h>

/* A 32-bit instruction word, little-endian (emit32 already writes LE). */
static void
w(struct code *c, uint32_t insn)
{
    emit32(c, insn);
}

/****************************************************************
 * Move wide immediate
 ****************************************************************/

/* opc: MOVN=00, MOVZ=10, MOVK=11.  hw = shift/16. */
static void
movw(struct code *c, int sf, int opc, int Rd, unsigned imm16, int shift)
{
    int hw = shift / 16;
    w(c, ((uint32_t)(sf & 1) << 31) | ((uint32_t)opc << 29) | (0x25u << 23) |
         ((uint32_t)(hw & 3) << 21) | ((imm16 & 0xffff) << 5) | (uint32_t)(Rd & 31));
}

void a_movz(struct code *c, int sf, int Rd, unsigned imm16, int shift) { movw(c, sf, 2, Rd, imm16, shift); }
void a_movk(struct code *c, int sf, int Rd, unsigned imm16, int shift) { movw(c, sf, 3, Rd, imm16, shift); }
void a_movn(struct code *c, int sf, int Rd, unsigned imm16, int shift) { movw(c, sf, 0, Rd, imm16, shift); }

void
a_movimm(struct code *c, int sf, int Rd, uint64_t v)
{
    int lanes = sf ? 4 : 2;
    int first = 1, k;

    if (sf == 0)
        v &= 0xffffffffu;
    /* movz the lowest non-zero lane, then movk each remaining non-zero lane;
       an all-zero value is a single movz #0. */
    for (k = 0; k < lanes; k++) {
        unsigned lane = (unsigned)((v >> (16 * k)) & 0xffff);
        if (lane == 0 && !(first && k == lanes - 1))
            continue;
        if (first) {
            a_movz(c, sf, Rd, lane, 16 * k);
            first = 0;
        } else {
            a_movk(c, sf, Rd, lane, 16 * k);
        }
    }
}

/****************************************************************
 * Add / subtract immediate
 ****************************************************************/

/* op: ADD=0, SUB=1.  S=0 (no flags).  Rd/Rn may be sp (31). */
static void
addsub_imm(struct code *c, int sf, int op, int Rd, int Rn, unsigned imm12, int lsl12)
{
    w(c, ((uint32_t)(sf & 1) << 31) | ((uint32_t)op << 30) | (0x11u << 24) |
         ((uint32_t)(lsl12 ? 1 : 0) << 22) | ((imm12 & 0xfff) << 10) |
         ((uint32_t)(Rn & 31) << 5) | (uint32_t)(Rd & 31));
}

void a_add_imm(struct code *c, int sf, int Rd, int Rn, unsigned imm12, int lsl12) { addsub_imm(c, sf, 0, Rd, Rn, imm12, lsl12); }
void a_sub_imm(struct code *c, int sf, int Rd, int Rn, unsigned imm12, int lsl12) { addsub_imm(c, sf, 1, Rd, Rn, imm12, lsl12); }

void
a_addimm(struct code *c, int Rd, int Rn, long imm, int scratch)
{
    long a = imm < 0 ? -imm : imm;
    int op = imm < 0 ? 1 : 0;

    if (imm == 0) {
        if (Rd != Rn)
            a_mov_reg(c, 1, Rd, Rn);
        return;
    }
    if (a <= 0xfff) {
        addsub_imm(c, 1, op, Rd, Rn, (unsigned)a, 0);
    } else if ((a & 0xfff) == 0 && (a >> 12) <= 0xfff) {
        addsub_imm(c, 1, op, Rd, Rn, (unsigned)(a >> 12), 1);
    } else {
        a_movimm(c, 1, scratch, (uint64_t)imm);
        a_add_reg(c, 1, Rd, Rn, scratch);
    }
}

/****************************************************************
 * Data-processing, three-register
 ****************************************************************/

/* Logical (shifted register), shift=00, imm6=0.  opc/N per op. */
static void
logic_reg(struct code *c, int sf, int opc, int N, int Rd, int Rn, int Rm)
{
    w(c, ((uint32_t)(sf & 1) << 31) | ((uint32_t)opc << 29) | (0x0Au << 24) |
         ((uint32_t)N << 21) | ((uint32_t)(Rm & 31) << 16) |
         ((uint32_t)(Rn & 31) << 5) | (uint32_t)(Rd & 31));
}

void a_and_reg(struct code *c, int sf, int Rd, int Rn, int Rm) { logic_reg(c, sf, 0, 0, Rd, Rn, Rm); }
void a_orr_reg(struct code *c, int sf, int Rd, int Rn, int Rm) { logic_reg(c, sf, 1, 0, Rd, Rn, Rm); }
void a_eor_reg(struct code *c, int sf, int Rd, int Rn, int Rm) { logic_reg(c, sf, 2, 0, Rd, Rn, Rm); }

void a_mov_reg(struct code *c, int sf, int Rd, int Rn) { logic_reg(c, sf, 1, 0, Rd, A_ZR, Rn); }   /* orr Rd, zr, Rn */
void a_mvn(struct code *c, int sf, int Rd, int Rm) { logic_reg(c, sf, 1, 1, Rd, A_ZR, Rm); }        /* orn Rd, zr, Rm */

/* Add/subtract (shifted register), shift=00, imm6=0. */
static void
addsub_reg(struct code *c, int sf, int op, int S, int Rd, int Rn, int Rm)
{
    w(c, ((uint32_t)(sf & 1) << 31) | ((uint32_t)op << 30) | ((uint32_t)S << 29) |
         (0x0Bu << 24) | ((uint32_t)(Rm & 31) << 16) |
         ((uint32_t)(Rn & 31) << 5) | (uint32_t)(Rd & 31));
}

void a_add_reg(struct code *c, int sf, int Rd, int Rn, int Rm) { addsub_reg(c, sf, 0, 0, Rd, Rn, Rm); }
void a_sub_reg(struct code *c, int sf, int Rd, int Rn, int Rm) { addsub_reg(c, sf, 1, 0, Rd, Rn, Rm); }
void a_neg(struct code *c, int sf, int Rd, int Rm) { addsub_reg(c, sf, 1, 0, Rd, A_ZR, Rm); }        /* sub Rd, zr, Rm */
void a_cmp_reg(struct code *c, int sf, int Rn, int Rm) { addsub_reg(c, sf, 1, 1, A_ZR, Rn, Rm); }     /* subs zr, Rn, Rm */

/* Data-processing (3 source): MADD/MSUB. o0: MADD=0, MSUB=1. */
static void
madd(struct code *c, int sf, int o0, int Rd, int Rn, int Rm, int Ra)
{
    w(c, ((uint32_t)(sf & 1) << 31) | (0x1Bu << 24) | ((uint32_t)(Rm & 31) << 16) |
         ((uint32_t)o0 << 15) | ((uint32_t)(Ra & 31) << 10) |
         ((uint32_t)(Rn & 31) << 5) | (uint32_t)(Rd & 31));
}

void a_mul(struct code *c, int sf, int Rd, int Rn, int Rm) { madd(c, sf, 0, Rd, Rn, Rm, A_ZR); }
void a_msub(struct code *c, int sf, int Rd, int Rn, int Rm, int Ra) { madd(c, sf, 1, Rd, Rn, Rm, Ra); }

/* Data-processing (2 source): SDIV/UDIV/LSLV/ASRV/LSRV.  opcode field [15:10]. */
static void
dp2(struct code *c, int sf, int opcode, int Rd, int Rn, int Rm)
{
    w(c, ((uint32_t)(sf & 1) << 31) | (0x0D6u << 21) | ((uint32_t)(Rm & 31) << 16) |
         ((uint32_t)opcode << 10) | ((uint32_t)(Rn & 31) << 5) | (uint32_t)(Rd & 31));
}

void a_udiv(struct code *c, int sf, int Rd, int Rn, int Rm) { dp2(c, sf, 0x02, Rd, Rn, Rm); }
void a_sdiv(struct code *c, int sf, int Rd, int Rn, int Rm) { dp2(c, sf, 0x03, Rd, Rn, Rm); }
void a_lslv(struct code *c, int sf, int Rd, int Rn, int Rm) { dp2(c, sf, 0x08, Rd, Rn, Rm); }
void a_lsrv(struct code *c, int sf, int Rd, int Rn, int Rm) { dp2(c, sf, 0x09, Rd, Rn, Rm); }
void a_asrv(struct code *c, int sf, int Rd, int Rn, int Rm) { dp2(c, sf, 0x0A, Rd, Rn, Rm); }

/* SBFM: sxtw Xd, Wn = SBFM Xd, Xn, #0, #31 (sf=1, N=1, immr=0, imms=31). */
void
a_sxtw(struct code *c, int Rd, int Rn)
{
    w(c, (1u << 31) | (0x26u << 23) | (1u << 22) | (0u << 16) | (31u << 10) |
         ((uint32_t)(Rn & 31) << 5) | (uint32_t)(Rd & 31));
}

/* CSINC Rd, Rn, Rm, cond; cset Rd, cc = csinc Rd, zr, zr, invert(cc). */
void
a_cset(struct code *c, int sf, int Rd, int cc)
{
    int inv = cc ^ 1;   /* invert the low bit to negate the condition */
    w(c, ((uint32_t)(sf & 1) << 31) | (0xD4u << 21) | ((uint32_t)A_ZR << 16) |
         ((uint32_t)(inv & 0xf) << 12) | (1u << 10) | ((uint32_t)A_ZR << 5) |
         (uint32_t)(Rd & 31));
}

/****************************************************************
 * Loads and stores
 ****************************************************************/

/* GPR load/store, base+offset.  size: 0=B,1=H,2=W,3=X.  opc: store=0,
   load(zero/plain)=1, load-signed-to-64=2, load-signed-to-32=3.  Picks the
   scaled unsigned-offset form, else the unscaled signed 9-bit form. */
static void
ldst_gpr(struct code *c, int size, int opc, int Rt, int Rn, int off)
{
    int scale = size;
    if (off >= 0 && (off & ((1 << scale) - 1)) == 0 && (off >> scale) <= 0xfff) {
        unsigned imm12 = (unsigned)(off >> scale);
        w(c, ((uint32_t)size << 30) | (0x39u << 24) | ((uint32_t)opc << 22) |
             (imm12 << 10) | ((uint32_t)(Rn & 31) << 5) | (uint32_t)(Rt & 31));
    } else {
        unsigned imm9 = (unsigned)(off & 0x1ff);
        w(c, ((uint32_t)size << 30) | (0x38u << 24) | ((uint32_t)opc << 22) |
             (imm9 << 12) | ((uint32_t)(Rn & 31) << 5) | (uint32_t)(Rt & 31));
    }
}

void a_strb(struct code *c, int Rt, int Rn, int off) { ldst_gpr(c, 0, 0, Rt, Rn, off); }
void a_strh(struct code *c, int Rt, int Rn, int off) { ldst_gpr(c, 1, 0, Rt, Rn, off); }
void a_strw(struct code *c, int Rt, int Rn, int off) { ldst_gpr(c, 2, 0, Rt, Rn, off); }
void a_strx(struct code *c, int Rt, int Rn, int off) { ldst_gpr(c, 3, 0, Rt, Rn, off); }
void a_ldrb(struct code *c, int Rt, int Rn, int off) { ldst_gpr(c, 0, 1, Rt, Rn, off); }
void a_ldrsb(struct code *c, int Rt, int Rn, int off) { ldst_gpr(c, 0, 3, Rt, Rn, off); }
void a_ldrh(struct code *c, int Rt, int Rn, int off) { ldst_gpr(c, 1, 1, Rt, Rn, off); }
void a_ldrsh(struct code *c, int Rt, int Rn, int off) { ldst_gpr(c, 1, 3, Rt, Rn, off); }
void a_ldrw(struct code *c, int Rt, int Rn, int off) { ldst_gpr(c, 2, 1, Rt, Rn, off); }
void a_ldrx(struct code *c, int Rt, int Rn, int off) { ldst_gpr(c, 3, 1, Rt, Rn, off); }

/* FP/SIMD scalar load/store.  sz: 1=H,2=S,3=D -> size field 1/2/3, V=1. */
static void
ldst_fp(struct code *c, int sz, int opc, int Rt, int Rn, int off)
{
    int scale = sz;
    if (off >= 0 && (off & ((1 << scale) - 1)) == 0 && (off >> scale) <= 0xfff) {
        unsigned imm12 = (unsigned)(off >> scale);
        w(c, ((uint32_t)sz << 30) | (0x3Du << 24) | ((uint32_t)opc << 22) |
             (imm12 << 10) | ((uint32_t)(Rn & 31) << 5) | (uint32_t)(Rt & 31));
    } else {
        unsigned imm9 = (unsigned)(off & 0x1ff);
        w(c, ((uint32_t)sz << 30) | (0x3Cu << 24) | ((uint32_t)opc << 22) |
             (imm9 << 12) | ((uint32_t)(Rn & 31) << 5) | (uint32_t)(Rt & 31));
    }
}

void a_str_fp(struct code *c, int sz, int Rt, int Rn, int off) { ldst_fp(c, sz, 0, Rt, Rn, off); }
void a_ldr_fp(struct code *c, int sz, int Rt, int Rn, int off) { ldst_fp(c, sz, 1, Rt, Rn, off); }

/****************************************************************
 * FP data processing
 ****************************************************************/

/* ftype: single=00, double=01, half=11 (from sz 2/3/1). */
static int
ftype(int sz)
{
    return sz == 3 ? 1 : sz == 1 ? 3 : 0;
}

/* FP data-processing (1 source): opcode [20:15]. */
static void
fp1(struct code *c, int sz, int opcode, int Rd, int Rn)
{
    w(c, (0x1Eu << 24) | ((uint32_t)ftype(sz) << 22) | (1u << 21) |
         ((uint32_t)opcode << 15) | (0x10u << 10) |
         ((uint32_t)(Rn & 31) << 5) | (uint32_t)(Rd & 31));
}

void a_fmov(struct code *c, int sz, int Rd, int Rn) { fp1(c, sz, 0x00, Rd, Rn); }
void a_fabs(struct code *c, int sz, int Rd, int Rn) { fp1(c, sz, 0x01, Rd, Rn); }
void a_fneg(struct code *c, int sz, int Rd, int Rn) { fp1(c, sz, 0x02, Rd, Rn); }
void a_fsqrt(struct code *c, int sz, int Rd, int Rn) { fp1(c, sz, 0x03, Rd, Rn); }

/* FCVT (1 source): opcode = 0001xx where xx selects the target type
   (single=00, double=01, half=11); ftype is the source. */
void
a_fcvt(struct code *c, int dstsz, int srcsz, int Rd, int Rn)
{
    int opcode = 0x04 | ftype(dstsz);
    fp1(c, srcsz, opcode, Rd, Rn);
}

/* FP data-processing (2 source): opcode [15:12].  MUL=0,DIV=1,ADD=2,SUB=3. */
static void
fp2(struct code *c, int sz, int opcode, int Rd, int Rn, int Rm)
{
    w(c, (0x1Eu << 24) | ((uint32_t)ftype(sz) << 22) | (1u << 21) |
         ((uint32_t)(Rm & 31) << 16) | ((uint32_t)opcode << 12) | (0x2u << 10) |
         ((uint32_t)(Rn & 31) << 5) | (uint32_t)(Rd & 31));
}

void a_fmul(struct code *c, int sz, int Rd, int Rn, int Rm) { fp2(c, sz, 0x0, Rd, Rn, Rm); }
void a_fdiv(struct code *c, int sz, int Rd, int Rn, int Rm) { fp2(c, sz, 0x1, Rd, Rn, Rm); }
void a_fadd(struct code *c, int sz, int Rd, int Rn, int Rm) { fp2(c, sz, 0x2, Rd, Rn, Rm); }
void a_fsub(struct code *c, int sz, int Rd, int Rn, int Rm) { fp2(c, sz, 0x3, Rd, Rn, Rm); }

/* FCMP Rn, Rm: FP compare, opcode2 = 00000. */
void
a_fcmp(struct code *c, int sz, int Rn, int Rm)
{
    w(c, (0x1Eu << 24) | ((uint32_t)ftype(sz) << 22) | (1u << 21) |
         ((uint32_t)(Rm & 31) << 16) | (0x8u << 10) |
         ((uint32_t)(Rn & 31) << 5) | 0u);
}

/* Conversion float<->int.  sf=0 (Wn/Wd int), type from sz, rmode/opcode per op. */
static void
fcvt_int(struct code *c, int sf, int sz, int rmode, int opcode, int Rd, int Rn)
{
    w(c, ((uint32_t)(sf & 1) << 31) | (0x1Eu << 24) | ((uint32_t)ftype(sz) << 22) |
         (1u << 21) | ((uint32_t)rmode << 19) | ((uint32_t)opcode << 16) |
         ((uint32_t)(Rn & 31) << 5) | (uint32_t)(Rd & 31));
}

void a_scvtf(struct code *c, int sz, int Rd, int Rn) { fcvt_int(c, 0, sz, 0, 2, Rd, Rn); }   /* Wn -> Sd/Dd */
void a_fcvtzs(struct code *c, int sz, int Rd, int Rn) { fcvt_int(c, 0, sz, 3, 0, Rd, Rn); }   /* Sn/Dn -> Wd, toward zero */
void a_fcvtns(struct code *c, int sz, int Rd, int Rn) { fcvt_int(c, 0, sz, 0, 0, Rd, Rn); }   /* Sn/Dn -> Wd, to nearest */

/****************************************************************
 * Control flow
 ****************************************************************/

size_t
a_b(struct code *c)
{
    size_t site = c->len;
    w(c, 0x14000000u);          /* b #0 */
    return site;
}

size_t
a_bl(struct code *c)
{
    size_t site = c->len;
    w(c, 0x94000000u);          /* bl #0 */
    return site;
}

size_t
a_bcond(struct code *c, int cc)
{
    size_t site = c->len;
    w(c, 0x54000000u | (uint32_t)(cc & 0xf));   /* b.cc #0 */
    return site;
}

size_t
a_cbz(struct code *c, int sf, int Rt)
{
    size_t site = c->len;
    w(c, ((uint32_t)(sf & 1) << 31) | 0x34000000u | (uint32_t)(Rt & 31));
    return site;
}

size_t
a_cbnz(struct code *c, int sf, int Rt)
{
    size_t site = c->len;
    w(c, ((uint32_t)(sf & 1) << 31) | 0x35000000u | (uint32_t)(Rt & 31));
    return site;
}

/* Patch a 26-bit branch (b / bl) at site to reach byte offset target. */
int
a_patch_imm26(struct code *c, size_t site, size_t target)
{
    long rel = ((long)target - (long)site) >> 2;
    uint32_t insn;
    if (rel < -(1L << 25) || rel >= (1L << 25))
        return -1;
    memcpy(&insn, c->buf + site, 4);
    insn = (insn & 0xFC000000u) | ((uint32_t)rel & 0x03FFFFFFu);
    memcpy(c->buf + site, &insn, 4);
    return 0;
}

/* Patch a 19-bit branch (b.cond / cbz / cbnz) at site to byte offset target. */
int
a_patch_imm19(struct code *c, size_t site, size_t target)
{
    long rel = ((long)target - (long)site) >> 2;
    uint32_t insn;
    if (rel < -(1L << 18) || rel >= (1L << 18))
        return -1;
    memcpy(&insn, c->buf + site, 4);
    insn = (insn & 0xFF00001Fu) | (((uint32_t)rel & 0x7FFFFu) << 5);
    memcpy(c->buf + site, &insn, 4);
    return 0;
}

void a_blr(struct code *c, int Rn) { w(c, 0xD63F0000u | ((uint32_t)(Rn & 31) << 5)); }
void a_br(struct code *c, int Rn) { w(c, 0xD61F0000u | ((uint32_t)(Rn & 31) << 5)); }
void a_ret(struct code *c) { w(c, 0xD65F0000u | ((uint32_t)A_LR << 5)); }
void a_nop(struct code *c) { w(c, 0xD503201Fu); }
