/* icov.c : which guest instructions a test actually executed */
/* Copyright (c) 2026 Jon Mayo - MIT-0 OR Public Domain */

/* Line coverage of an interpreter answers "which of my C did you run".
 * For an emulator the question that matters is "which of the instruction
 * set did you run", and the two are not the same: one switch arm can serve
 * eight instructions, so a method can execute every line of the decoder
 * while touching a third of the architecture.
 *
 * This is the second measure. rv32.c calls rv_icov_note() once per
 * instruction when built with -DRV_ICOV, and the classifier below turns an
 * encoding into one of the named instructions this emulator implements.
 * Nothing here is on the path of an ordinary build. */

#include "icov.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/****************************************************************
 * The instruction table
 *
 * One entry per instruction the emulator implements, in the order a
 * report reads best. The classifier returns an index into this.
 ****************************************************************/

static const char *const names[] = {
#define X(n) #n,
    /* RV32I */
    X(lui) X(auipc) X(jal) X(jalr)
    X(beq) X(bne) X(blt) X(bge) X(bltu) X(bgeu)
    X(lb) X(lh) X(lw) X(lbu) X(lhu)
    X(sb) X(sh) X(sw)
    X(addi) X(slti) X(sltiu) X(xori) X(ori) X(andi)
    X(slli) X(srli) X(srai)
    X(add) X(sub) X(sll) X(slt) X(sltu) X(xor) X(srl) X(sra) X(or) X(and)
    X(fence) X(fence_i) X(ecall) X(ebreak) X(mret) X(wfi)
    X(csrrw) X(csrrs) X(csrrc) X(csrrwi) X(csrrsi) X(csrrci)
    /* M */
    X(mul) X(mulh) X(mulhsu) X(mulhu) X(div) X(divu) X(rem) X(remu)
    /* A */
    X(lr_w) X(sc_w) X(amoswap_w) X(amoadd_w) X(amoxor_w) X(amoand_w)
    X(amoor_w) X(amomin_w) X(amomax_w) X(amominu_w) X(amomaxu_w)
    /* F */
    X(flw) X(fsw)
    X(fadd_s) X(fsub_s) X(fmul_s) X(fdiv_s) X(fsqrt_s)
    X(fsgnj_s) X(fsgnjn_s) X(fsgnjx_s) X(fmin_s) X(fmax_s)
    X(feq_s) X(flt_s) X(fle_s) X(fclass_s)
    X(fcvt_w_s) X(fcvt_wu_s) X(fcvt_s_w) X(fcvt_s_wu)
    X(fmv_x_w) X(fmv_w_x)
    X(fmadd_s) X(fmsub_s) X(fnmsub_s) X(fnmadd_s)
    /* Zba, Zbb, Zbs */
    X(sh1add) X(sh2add) X(sh3add)
    X(andn) X(orn) X(xnor)
    X(min) X(minu) X(max) X(maxu)
    X(clz) X(ctz) X(cpop) X(sext_b) X(sext_h) X(zext_h)
    X(rol) X(ror) X(rori) X(orc_b) X(rev8)
    X(bset) X(bclr) X(binv) X(bext)
    X(bseti) X(bclri) X(binvi) X(bexti)
    /* Compressed, counted as themselves rather than as what they expand
     * into, because a decoder can get the expansion wrong for one form
     * while every 32-bit instruction it produces is already covered. */
    X(c_addi4spn) X(c_lw) X(c_flw) X(c_sw) X(c_fsw)
    X(c_nop) X(c_addi) X(c_jal) X(c_li) X(c_addi16sp) X(c_lui)
    X(c_srli) X(c_srai) X(c_andi)
    X(c_sub) X(c_xor) X(c_or) X(c_and)
    X(c_j) X(c_beqz) X(c_bnez)
    X(c_slli) X(c_lwsp) X(c_flwsp) X(c_jr) X(c_mv) X(c_ebreak)
    X(c_jalr) X(c_add) X(c_swsp) X(c_fswsp)
    /* Zcb */
    X(c_lbu) X(c_lhu) X(c_lh) X(c_sb) X(c_sh)
    X(c_zext_b) X(c_sext_b) X(c_zext_h) X(c_sext_h) X(c_not) X(c_mul)
    /* Zcmp */
    X(cm_push) X(cm_pop) X(cm_popret) X(cm_popretz) X(cm_mva01s) X(cm_mvsa01)
#undef X
};

#define ICOV_N ((int)(sizeof(names) / sizeof(names[0])))

static uint64_t counts[ICOV_N];

/* Index lookup by name, resolved once. Slow and used only at startup of a
 * classification, which happens per instruction, so the classifier returns
 * indices directly instead. These enum values shadow the table order. */
enum {
#define X(n) IX_##n,
    X(lui) X(auipc) X(jal) X(jalr)
    X(beq) X(bne) X(blt) X(bge) X(bltu) X(bgeu)
    X(lb) X(lh) X(lw) X(lbu) X(lhu)
    X(sb) X(sh) X(sw)
    X(addi) X(slti) X(sltiu) X(xori) X(ori) X(andi)
    X(slli) X(srli) X(srai)
    X(add) X(sub) X(sll) X(slt) X(sltu) X(xor) X(srl) X(sra) X(or) X(and)
    X(fence) X(fence_i) X(ecall) X(ebreak) X(mret) X(wfi)
    X(csrrw) X(csrrs) X(csrrc) X(csrrwi) X(csrrsi) X(csrrci)
    X(mul) X(mulh) X(mulhsu) X(mulhu) X(div) X(divu) X(rem) X(remu)
    X(lr_w) X(sc_w) X(amoswap_w) X(amoadd_w) X(amoxor_w) X(amoand_w)
    X(amoor_w) X(amomin_w) X(amomax_w) X(amominu_w) X(amomaxu_w)
    X(flw) X(fsw)
    X(fadd_s) X(fsub_s) X(fmul_s) X(fdiv_s) X(fsqrt_s)
    X(fsgnj_s) X(fsgnjn_s) X(fsgnjx_s) X(fmin_s) X(fmax_s)
    X(feq_s) X(flt_s) X(fle_s) X(fclass_s)
    X(fcvt_w_s) X(fcvt_wu_s) X(fcvt_s_w) X(fcvt_s_wu)
    X(fmv_x_w) X(fmv_w_x)
    X(fmadd_s) X(fmsub_s) X(fnmsub_s) X(fnmadd_s)
    X(sh1add) X(sh2add) X(sh3add)
    X(andn) X(orn) X(xnor)
    X(min) X(minu) X(max) X(maxu)
    X(clz) X(ctz) X(cpop) X(sext_b) X(sext_h) X(zext_h)
    X(rol) X(ror) X(rori) X(orc_b) X(rev8)
    X(bset) X(bclr) X(binv) X(bext)
    X(bseti) X(bclri) X(binvi) X(bexti)
    X(c_addi4spn) X(c_lw) X(c_flw) X(c_sw) X(c_fsw)
    X(c_nop) X(c_addi) X(c_jal) X(c_li) X(c_addi16sp) X(c_lui)
    X(c_srli) X(c_srai) X(c_andi)
    X(c_sub) X(c_xor) X(c_or) X(c_and)
    X(c_j) X(c_beqz) X(c_bnez)
    X(c_slli) X(c_lwsp) X(c_flwsp) X(c_jr) X(c_mv) X(c_ebreak)
    X(c_jalr) X(c_add) X(c_swsp) X(c_fswsp)
    X(c_lbu) X(c_lhu) X(c_lh) X(c_sb) X(c_sh)
    X(c_zext_b) X(c_sext_b) X(c_zext_h) X(c_sext_h) X(c_not) X(c_mul)
    X(cm_push) X(cm_pop) X(cm_popret) X(cm_popretz) X(cm_mva01s) X(cm_mvsa01)
#undef X
    IX_none
};

/****************************************************************
 * Classification
 ****************************************************************/

static int
classify_c(uint16_t c)
{
    uint32_t op = c & 3, f3 = (c >> 13) & 7;

    if (c == 0)
        return -1;

    if (op == 0) {
        switch (f3) {
        case 0: return IX_c_addi4spn;
        case 2: return IX_c_lw;
        case 3: return IX_c_flw;
        case 4:                                 /* Zcb */
            if (c & 0x1000)
                return -1;
            switch ((c >> 10) & 3) {
            case 0: return IX_c_lbu;
            case 1: return (c >> 6) & 1 ? IX_c_lh : IX_c_lhu;
            case 2: return IX_c_sb;
            default: return IX_c_sh;
            }
        case 6: return IX_c_sw;
        case 7: return IX_c_fsw;
        default: return -1;
        }
    }
    if (op == 1) {
        switch (f3) {
        case 0: return (c >> 7 & 0x1f) == 0 ? IX_c_nop : IX_c_addi;
        case 1: return IX_c_jal;
        case 2: return IX_c_li;
        case 3: return (c >> 7 & 0x1f) == 2 ? IX_c_addi16sp : IX_c_lui;
        case 4:
            switch ((c >> 10) & 7) {
            case 0: case 4: return IX_c_srli;
            case 1: case 5: return IX_c_srai;
            case 2: case 6: return IX_c_andi;
            case 3:
                switch ((c >> 5) & 3) {
                case 0: return IX_c_sub;
                case 1: return IX_c_xor;
                case 2: return IX_c_or;
                default: return IX_c_and;
                }
            default:                            /* 7: Zcb quadrant 1 */
                if (((c >> 5) & 3) == 2)
                    return IX_c_mul;
                switch ((c >> 2) & 7) {
                case 0: return IX_c_zext_b;
                case 1: return IX_c_sext_b;
                case 2: return IX_c_zext_h;
                case 3: return IX_c_sext_h;
                case 5: return IX_c_not;
                default: return -1;
                }
            }
        case 5: return IX_c_j;
        case 6: return IX_c_beqz;
        default: return IX_c_bnez;
        }
    }
    /* Quadrant 2 */
    switch (f3) {
    case 0: return IX_c_slli;
    case 2: return IX_c_lwsp;
    case 3: return IX_c_flwsp;
    case 4:
        if (c & 0x1000) {
            if ((c >> 2 & 0x1f) == 0)
                return (c >> 7 & 0x1f) == 0 ? IX_c_ebreak : IX_c_jalr;
            return IX_c_add;
        }
        return (c >> 2 & 0x1f) == 0 ? IX_c_jr : IX_c_mv;
    case 6: return IX_c_swsp;
    case 7: return IX_c_fswsp;
    default: return -1;
    }
}

static int
classify_zcmp(uint16_t c)
{
    uint32_t funct5 = (c >> 8) & 0x1f;

    if ((c & 0xe003) != 0xa002)
        return -1;
    switch (funct5) {
    case 0x18: return IX_cm_push;
    case 0x1a: return IX_cm_pop;
    case 0x1c: return IX_cm_popretz;
    case 0x1e: return IX_cm_popret;
    default:
        if (((c >> 10) & 3) == 3)
            return ((c >> 5) & 3) == 3 ? IX_cm_mva01s : IX_cm_mvsa01;
        return -1;
    }
}

static int
classify_zb_reg(uint32_t f7, uint32_t f3, uint32_t rs2)
{
    switch (f7) {
    case 0x04: return f3 == 4 && rs2 == 0 ? IX_zext_h : -1;
    case 0x05:
        switch (f3) {
        case 4: return IX_min;
        case 5: return IX_minu;
        case 6: return IX_max;
        case 7: return IX_maxu;
        default: return -1;
        }
    case 0x10:
        switch (f3) {
        case 2: return IX_sh1add;
        case 4: return IX_sh2add;
        case 6: return IX_sh3add;
        default: return -1;
        }
    case 0x14: return f3 == 1 ? IX_bset : -1;
    case 0x20:
        switch (f3) {
        case 4: return IX_xnor;
        case 6: return IX_orn;
        case 7: return IX_andn;
        default: return -1;
        }
    case 0x24: return f3 == 1 ? IX_bclr : f3 == 5 ? IX_bext : -1;
    case 0x30: return f3 == 1 ? IX_rol : f3 == 5 ? IX_ror : -1;
    case 0x34: return f3 == 1 ? IX_binv : -1;
    default: return -1;
    }
}

static int
classify_zb_imm(uint32_t f7, uint32_t f3, uint32_t shamt)
{
    switch (f7) {
    case 0x14:
        if (f3 == 1) return IX_bseti;
        return f3 == 5 && shamt == 7 ? IX_orc_b : -1;
    case 0x24: return f3 == 1 ? IX_bclri : f3 == 5 ? IX_bexti : -1;
    case 0x30:
        if (f3 == 5) return IX_rori;
        switch (shamt) {
        case 0: return IX_clz;
        case 1: return IX_ctz;
        case 2: return IX_cpop;
        case 4: return IX_sext_b;
        case 5: return IX_sext_h;
        default: return -1;
        }
    case 0x34:
        if (f3 == 1) return IX_binvi;
        return f3 == 5 && shamt == 0x18 ? IX_rev8 : -1;
    default: return -1;
    }
}

static int
classify(uint32_t insn, int len)
{
    uint32_t op, f3, f7, rs2;
    int ix;

    if (len == 2) {
        ix = classify_zcmp((uint16_t)insn);
        return ix >= 0 ? ix : classify_c((uint16_t)insn);
    }

    op = insn & 0x7f;
    f3 = (insn >> 12) & 7;
    f7 = (insn >> 25) & 0x7f;
    rs2 = (insn >> 20) & 0x1f;

    switch (op) {
    case 0x37: return IX_lui;
    case 0x17: return IX_auipc;
    case 0x6f: return IX_jal;
    case 0x67: return IX_jalr;
    case 0x63:
        switch (f3) {
        case 0: return IX_beq;
        case 1: return IX_bne;
        case 4: return IX_blt;
        case 5: return IX_bge;
        case 6: return IX_bltu;
        case 7: return IX_bgeu;
        default: return -1;
        }
    case 0x03:
        switch (f3) {
        case 0: return IX_lb;
        case 1: return IX_lh;
        case 2: return IX_lw;
        case 4: return IX_lbu;
        case 5: return IX_lhu;
        default: return -1;
        }
    case 0x23:
        switch (f3) {
        case 0: return IX_sb;
        case 1: return IX_sh;
        case 2: return IX_sw;
        default: return -1;
        }
    case 0x07: return f3 == 2 ? IX_flw : -1;
    case 0x27: return f3 == 2 ? IX_fsw : -1;
    case 0x13:
        switch (f3) {
        case 0: return IX_addi;
        case 2: return IX_slti;
        case 3: return IX_sltiu;
        case 4: return IX_xori;
        case 6: return IX_ori;
        case 7: return IX_andi;
        case 1:
            if (f7 == 0) return IX_slli;
            return classify_zb_imm(f7, f3, rs2);
        default:
            if (f7 == 0) return IX_srli;
            if (f7 == 0x20) return IX_srai;
            return classify_zb_imm(f7, f3, rs2);
        }
    case 0x33:
        if (f7 == 0x01) {
            switch (f3) {
            case 0: return IX_mul;
            case 1: return IX_mulh;
            case 2: return IX_mulhsu;
            case 3: return IX_mulhu;
            case 4: return IX_div;
            case 5: return IX_divu;
            case 6: return IX_rem;
            default: return IX_remu;
            }
        }
        if (f7 != 0 && f7 != 0x20)
            return classify_zb_reg(f7, f3, rs2);
        switch (f3) {
        case 0: return f7 == 0x20 ? IX_sub : IX_add;
        case 1: return IX_sll;
        case 2: return IX_slt;
        case 3: return IX_sltu;
        case 4: return f7 == 0 ? IX_xor : classify_zb_reg(f7, f3, rs2);
        case 5: return f7 == 0x20 ? IX_sra : IX_srl;
        case 6: return f7 == 0 ? IX_or : classify_zb_reg(f7, f3, rs2);
        default: return f7 == 0 ? IX_and : classify_zb_reg(f7, f3, rs2);
        }
    case 0x0f: return f3 == 1 ? IX_fence_i : IX_fence;
    case 0x73:
        switch (f3) {
        case 0:
            if (insn == 0x00000073) return IX_ecall;
            if (insn == 0x00100073) return IX_ebreak;
            if (insn == 0x30200073) return IX_mret;
            if (insn == 0x10500073) return IX_wfi;
            return -1;
        case 1: return IX_csrrw;
        case 2: return IX_csrrs;
        case 3: return IX_csrrc;
        case 5: return IX_csrrwi;
        case 6: return IX_csrrsi;
        case 7: return IX_csrrci;
        default: return -1;
        }
    case 0x2f: {
        uint32_t f5 = (insn >> 27) & 0x1f;

        switch (f5) {
        case 0x00: return IX_amoadd_w;
        case 0x01: return IX_amoswap_w;
        case 0x02: return IX_lr_w;
        case 0x03: return IX_sc_w;
        case 0x04: return IX_amoxor_w;
        case 0x08: return IX_amoor_w;
        case 0x0c: return IX_amoand_w;
        case 0x10: return IX_amomin_w;
        case 0x14: return IX_amomax_w;
        case 0x18: return IX_amominu_w;
        case 0x1c: return IX_amomaxu_w;
        default: return -1;
        }
    }
    case 0x53:
        switch (f7) {
        case 0x00: return IX_fadd_s;
        case 0x04: return IX_fsub_s;
        case 0x08: return IX_fmul_s;
        case 0x0c: return IX_fdiv_s;
        case 0x2c: return IX_fsqrt_s;
        case 0x10:
            return f3 == 0 ? IX_fsgnj_s : f3 == 1 ? IX_fsgnjn_s : IX_fsgnjx_s;
        case 0x14: return f3 == 0 ? IX_fmin_s : IX_fmax_s;
        case 0x50:
            return f3 == 0 ? IX_fle_s : f3 == 1 ? IX_flt_s : IX_feq_s;
        case 0x60: return rs2 == 0 ? IX_fcvt_w_s : IX_fcvt_wu_s;
        case 0x68: return rs2 == 0 ? IX_fcvt_s_w : IX_fcvt_s_wu;
        case 0x70: return f3 == 0 ? IX_fmv_x_w : IX_fclass_s;
        case 0x78: return IX_fmv_w_x;
        default: return -1;
        }
    case 0x43: return IX_fmadd_s;
    case 0x47: return IX_fmsub_s;
    case 0x4b: return IX_fnmsub_s;
    case 0x4f: return IX_fnmadd_s;
    default: return -1;
    }
}

/****************************************************************
 * The hook and the report
 ****************************************************************/

/* Writing the table out at exit means no tool needs to know this exists:
 * anything linked with icov.c and run with RV_ICOV_OUT set leaves its
 * instruction set behind. The compliance runner forks a process per test,
 * so the name carries the pid and the driver merges what it finds. */
static int installed;

static void
icov_atexit(void)
{
    const char *dir = getenv("RV_ICOV_OUT");
    char path[512];
    FILE *f;

    if (!dir)
        return;
    snprintf(path, sizeof(path), "%s.%ld", dir, (long)getpid());
    f = fopen(path, "w");
    if (!f)
        return;
    rv_icov_dump(f);
    fclose(f);
}

void
rv_icov_note(uint32_t insn, int len)
{
    int ix = classify(insn, len);

    if (!installed) {
        installed = 1;
        atexit(icov_atexit);
    }
    if (ix >= 0 && ix < ICOV_N)
        counts[ix]++;
}

void
rv_icov_reset(void)
{
    memset(counts, 0, sizeof(counts));
}

int
rv_icov_total(void)
{
    return ICOV_N;
}

int
rv_icov_seen(void)
{
    int i, n = 0;

    for (i = 0; i < ICOV_N; i++)
        if (counts[i])
            n++;
    return n;
}

void
rv_icov_report(FILE *out, int list_missing)
{
    int i, n = rv_icov_seen();

    fprintf(out, "instructions executed: %d of %d (%.1f%%)\n",
            n, ICOV_N, n * 100.0 / ICOV_N);
    if (!list_missing)
        return;
    fprintf(out, "never executed:");
    for (i = 0; i < ICOV_N; i++)
        if (!counts[i])
            fprintf(out, " %s", names[i]);
    fprintf(out, "\n");
}

void
rv_icov_dump(FILE *out)
{
    int i;

    for (i = 0; i < ICOV_N; i++)
        fprintf(out, "%s %llu\n", names[i], (unsigned long long)counts[i]);
}
