/* mips_encode.c : MIPS I instruction encoder
 *
 * Covers the base integer instructions the backend emits (SPECIAL R-type, the
 * I-type arithmetic, loads and stores, branches and jumps) and the COP1
 * (coprocessor 1) floating-point instructions.  This is the real, non-macro
 * instruction set; the macros the backend leans on (li, la, move, mul, div,
 * rem, the branch pseudos, l.d, s.d) and the .set reorder delay-slot engine
 * are added in later steps.
 *
 * Every instruction is one 32-bit little-endian word.  An intra-section
 * branch or jump target is resolved to its final displacement in pass 2; a
 * cross-section or external target emits a relocation, matching what GNU as
 * writes before the linker patches it. */

#include "mips.h"

#include <stdlib.h>
#include <string.h>

/****************************************************************
 * Register names
 ****************************************************************/

int
mips_ireg(const char *name)
{
    static const char *const abi[32] = {
        "$zero", "$at", "$v0", "$v1", "$a0", "$a1", "$a2", "$a3",
        "$t0",   "$t1", "$t2", "$t3", "$t4", "$t5", "$t6", "$t7",
        "$s0",   "$s1", "$s2", "$s3", "$s4", "$s5", "$s6", "$s7",
        "$t8",   "$t9", "$k0", "$k1", "$gp", "$sp", "$fp", "$ra",
    };
    int k;

    if (name[0] != '$')
        return -1;
    for (k = 0; k < 32; k++)
        if (strcmp(name, abi[k]) == 0)
            return k;
    if (strcmp(name, "$s8") == 0)       /* $fp is register 30, also named $s8 */
        return 30;
    if (name[1] >= '0' && name[1] <= '9') {
        char *end;
        long n = strtol(name + 1, &end, 10);
        if (*end == '\0' && n >= 0 && n < 32)
            return (int)n;
    }
    return -1;
}

int
mips_freg(const char *name)
{
    if (name[0] == '$' && name[1] == 'f' && name[2] >= '0' && name[2] <= '9') {
        char *end;
        long n = strtol(name + 2, &end, 10);
        if (*end == '\0' && n >= 0 && n < 32)
            return (int)n;
    }
    return -1;
}

/****************************************************************
 * Instruction-format field packers
 ****************************************************************/

static uint32_t
enc_r(int op, int rs, int rt, int rd, int shamt, int funct)
{
    return ((uint32_t)(op & 63) << 26) | ((uint32_t)(rs & 31) << 21) |
           ((uint32_t)(rt & 31) << 16) | ((uint32_t)(rd & 31) << 11) |
           ((uint32_t)(shamt & 31) << 6) | (uint32_t)(funct & 63);
}

static uint32_t
enc_i(int op, int rs, int rt, int imm)
{
    return ((uint32_t)(op & 63) << 26) | ((uint32_t)(rs & 31) << 21) |
           ((uint32_t)(rt & 31) << 16) | (uint32_t)(imm & 0xffff);
}

static uint32_t
enc_j(int op, uint32_t target)
{
    return ((uint32_t)(op & 63) << 26) | (target & 0x3ffffff);
}

/****************************************************************
 * Emission and relocation helpers
 ****************************************************************/

static struct section *
cur(struct mips_asm *a)
{
    return &a->sections[a->cur_section];
}

static void
emit_word(struct mips_asm *a, uint32_t w)
{
    sec_emit32(cur(a), w);
}

static void
reloc_sym(struct mips_asm *a, uint32_t off, const char *name,
          int32_t addend, int type)
{
    int idx = sym_lookup(&a->st, name);
    if (idx < 0)
        idx = sym_add(&a->st, name);
    sec_add_reloc_t(cur(a), off, idx, addend, type);
}

/* Word-count displacement for a branch target, relative to the delay slot
 * (here + 4).  A target defined in the current section is final; otherwise a
 * PC16 relocation is emitted and the field left 0, as GNU as does. */
static int
branch_disp(struct mips_asm *a, const char *target, uint32_t here)
{
    int idx = sym_lookup(&a->st, target);
    int defined = idx >= 0 && a->st.syms[idx].defined;
    int same = defined && a->st.syms[idx].section == a->cur_section;
    int32_t disp;

    if (!same) {
        /* REL: the in-place field is the addend the linker adds before the
         * PC-relative subtraction.  GNU as writes -1 (one word) so the final
         * displacement is measured from the delay slot (here + 4). */
        reloc_sym(a, here, target, 0, R_MIPS_PC16);
        return -1;
    }
    disp = ((int32_t)a->st.syms[idx].value - (int32_t)(here + 4)) >> 2;
    if (disp < -32768 || disp > 32767)
        die("branch to '%s' out of range (displacement %d)", target, disp);
    return disp;
}

/* 26-bit word address for a jump target.  A jump is absolute, so its final
 * address is not known until the section is placed: an R_MIPS_26 relocation is
 * always emitted, even for a same-section target (GNU as does the same).  The
 * field is left 0; for a local symbol the ELF writer folds the word address
 * in-place, and for an external one the linker fills it. */
static uint32_t
jump_target(struct mips_asm *a, const char *target, uint32_t here)
{
    reloc_sym(a, here, target, 0, R_MIPS_26);
    return 0;
}

/****************************************************************
 * Operand accessors with validation
 ****************************************************************/

#define NEED(cond) do { if (!(cond)) return -1; } while (0)

static int
is_reg(struct mips_operand *o) { return o && o->kind == MO_REG; }
static int
is_freg(struct mips_operand *o) { return o && o->kind == MO_FREG; }
static int
is_imm(struct mips_operand *o) { return o && o->kind == MO_IMM; }
static int
is_mem(struct mips_operand *o) { return o && o->kind == MO_MEM; }

/* The symbol name for an operand in a symbol (branch/jump) position.  A bare
 * symbol is obvious; a register-spelled name is a symbol here too (a C global
 * that happens to look like a register), so its kept spelling is used. */
static const char *
sym_target(struct mips_operand *o)
{
    if (!o)
        return NULL;
    if (o->kind == MO_SYM || o->kind == MO_REG || o->kind == MO_FREG)
        return o->sym;
    return NULL;
}

/****************************************************************
 * Integer encoders
 ****************************************************************/

/* SPECIAL R-type, three registers: rd, rs, rt.  Operand order rd, rs, rt. */
static int
enc_rrr(const char *m, struct mips_operand *ops, int nops, uint32_t *w)
{
    int funct = -1;

    NEED(nops == 3 && is_reg(&ops[0]) && is_reg(&ops[1]) && is_reg(&ops[2]));

    if (strcmp(m, "addu") == 0) funct = 0x21;
    else if (strcmp(m, "subu") == 0) funct = 0x23;
    else if (strcmp(m, "add") == 0)  funct = 0x20;
    else if (strcmp(m, "sub") == 0)  funct = 0x22;
    else if (strcmp(m, "and") == 0)  funct = 0x24;
    else if (strcmp(m, "or") == 0)   funct = 0x25;
    else if (strcmp(m, "xor") == 0)  funct = 0x26;
    else if (strcmp(m, "nor") == 0)  funct = 0x27;
    else if (strcmp(m, "slt") == 0)  funct = 0x2a;
    else if (strcmp(m, "sltu") == 0) funct = 0x2b;
    else return -1;

    *w = enc_r(0, ops[1].reg, ops[2].reg, ops[0].reg, 0, funct);
    return 4;
}

/* Variable shift: rd, rt, rs (shift amount in rs).  sllv/srlv/srav. */
static int
enc_shiftv(const char *m, struct mips_operand *ops, int nops, uint32_t *w)
{
    int funct = -1;

    NEED(nops == 3 && is_reg(&ops[0]) && is_reg(&ops[1]) && is_reg(&ops[2]));

    if (strcmp(m, "sllv") == 0) funct = 0x04;
    else if (strcmp(m, "srlv") == 0) funct = 0x06;
    else if (strcmp(m, "srav") == 0) funct = 0x07;
    else return -1;

    /* rd = ops[0], rt = value (ops[1]), rs = shift (ops[2]) */
    *w = enc_r(0, ops[2].reg, ops[1].reg, ops[0].reg, 0, funct);
    return 4;
}

/* Immediate shift: rd, rt, shamt.  sll/srl/sra. */
static int
enc_shifti(const char *m, struct mips_operand *ops, int nops, uint32_t *w)
{
    int funct = -1;

    NEED(nops == 3 && is_reg(&ops[0]) && is_reg(&ops[1]) && is_imm(&ops[2]));

    if (strcmp(m, "sll") == 0) funct = 0x00;
    else if (strcmp(m, "srl") == 0) funct = 0x02;
    else if (strcmp(m, "sra") == 0) funct = 0x03;
    else return -1;

    *w = enc_r(0, 0, ops[1].reg, ops[0].reg, (int)ops[2].imm & 31, funct);
    return 4;
}

/* mult/multu/div/divu: two source registers, no destination. */
static int
enc_muldiv(const char *m, struct mips_operand *ops, int nops, uint32_t *w)
{
    int funct = -1;

    NEED(nops == 2 && is_reg(&ops[0]) && is_reg(&ops[1]));

    if (strcmp(m, "mult") == 0) funct = 0x18;
    else if (strcmp(m, "multu") == 0) funct = 0x19;
    else if (strcmp(m, "div") == 0) funct = 0x1a;
    else if (strcmp(m, "divu") == 0) funct = 0x1b;
    else return -1;

    *w = enc_r(0, ops[0].reg, ops[1].reg, 0, 0, funct);
    return 4;
}

/* mfhi/mflo (one destination), mthi/mtlo (one source). */
static int
enc_hilo(const char *m, struct mips_operand *ops, int nops, uint32_t *w)
{
    NEED(nops == 1 && is_reg(&ops[0]));

    if (strcmp(m, "mfhi") == 0) *w = enc_r(0, 0, 0, ops[0].reg, 0, 0x10);
    else if (strcmp(m, "mflo") == 0) *w = enc_r(0, 0, 0, ops[0].reg, 0, 0x12);
    else if (strcmp(m, "mthi") == 0) *w = enc_r(0, ops[0].reg, 0, 0, 0, 0x11);
    else if (strcmp(m, "mtlo") == 0) *w = enc_r(0, ops[0].reg, 0, 0, 0, 0x13);
    else return -1;
    return 4;
}

/* I-type arithmetic: rt, rs, imm.  addiu/addi/slti/sltiu/andi/ori/xori.  The
 * immediate may be %hi(sym)/%lo(sym), which emit a relocation; this is how the
 * la macro's second instruction (addiu rt, rt, %lo(sym)) lowers. */
static int
enc_arithi(struct mips_asm *a, const char *m, struct mips_operand *ops,
           int nops, int emit)
{
    int op = -1;

    if (strcmp(m, "addiu") == 0) op = 0x09;
    else if (strcmp(m, "addi") == 0) op = 0x08;
    else if (strcmp(m, "slti") == 0) op = 0x0a;
    else if (strcmp(m, "sltiu") == 0) op = 0x0b;
    else if (strcmp(m, "andi") == 0) op = 0x0c;
    else if (strcmp(m, "ori") == 0) op = 0x0d;
    else if (strcmp(m, "xori") == 0) op = 0x0e;
    else return -1;

    NEED(nops == 3 && is_reg(&ops[0]) && is_reg(&ops[1]) && is_imm(&ops[2]));

    if (emit) {
        if (ops[2].reloc_op == MRELOC_OP_HI)
            reloc_sym(a, (uint32_t)cur(a)->len, ops[2].sym,
                (int32_t)ops[2].imm, R_MIPS_HI16);
        else if (ops[2].reloc_op == MRELOC_OP_LO)
            reloc_sym(a, (uint32_t)cur(a)->len, ops[2].sym,
                (int32_t)ops[2].imm, R_MIPS_LO16);
        emit_word(a, enc_i(op, ops[1].reg, ops[0].reg,
            ops[2].reloc_op == MRELOC_OP_NONE ? (int)ops[2].imm : 0));
    }
    return 4;
}

/* lui: rt, imm (or %hi(sym)). */
static int
enc_lui(struct mips_asm *a, struct mips_operand *ops, int nops, int emit)
{
    NEED(nops == 2 && is_reg(&ops[0]) && is_imm(&ops[1]));

    if (emit) {
        if (ops[1].reloc_op == MRELOC_OP_HI)
            reloc_sym(a, (uint32_t)cur(a)->len, ops[1].sym,
                (int32_t)ops[1].imm, R_MIPS_HI16);
        emit_word(a, enc_i(0x0f, 0, ops[0].reg,
            ops[1].reloc_op == MRELOC_OP_NONE ? (int)ops[1].imm : 0));
    }
    return 4;
}

/* Loads and stores: rt, offset(base).  The offset may be a plain immediate or
 * %lo(sym). */
static int
enc_memop(struct mips_asm *a, const char *m, struct mips_operand *ops,
          int nops, int emit)
{
    int op = -1, cop = 0;

    if (strcmp(m, "lb") == 0) op = 0x20;
    else if (strcmp(m, "lh") == 0) op = 0x21;
    else if (strcmp(m, "lw") == 0) op = 0x23;
    else if (strcmp(m, "lbu") == 0) op = 0x24;
    else if (strcmp(m, "lhu") == 0) op = 0x25;
    else if (strcmp(m, "sb") == 0) op = 0x28;
    else if (strcmp(m, "sh") == 0) op = 0x29;
    else if (strcmp(m, "sw") == 0) op = 0x2b;
    else if (strcmp(m, "lwc1") == 0) { op = 0x31; cop = 1; }
    else if (strcmp(m, "swc1") == 0) { op = 0x39; cop = 1; }
    else return -1;

    /* The transferred register is an integer register, or a float register
     * for the coprocessor loads and stores; its number fills the rt field. */
    NEED(nops == 2 && is_mem(&ops[1]));
    NEED(cop ? is_freg(&ops[0]) : is_reg(&ops[0]));

    if (emit) {
        if (ops[1].reloc_op == MRELOC_OP_LO)
            reloc_sym(a, (uint32_t)cur(a)->len, ops[1].sym,
                (int32_t)ops[1].imm, R_MIPS_LO16);
        emit_word(a, enc_i(op, ops[1].reg, ops[0].reg,
            ops[1].reloc_op == MRELOC_OP_NONE ? (int)ops[1].imm : 0));
    }
    return 4;
}

/* Conditional branches.  beq/bne take rs, rt, target; blez/bgtz take rs,
 * target (rt = 0). */
static int
enc_branch(struct mips_asm *a, const char *m, struct mips_operand *ops,
           int nops, int emit)
{
    int op = -1, two_reg = 0;

    if (strcmp(m, "beq") == 0) { op = 0x04; two_reg = 1; }
    else if (strcmp(m, "bne") == 0) { op = 0x05; two_reg = 1; }
    else if (strcmp(m, "blez") == 0) op = 0x06;
    else if (strcmp(m, "bgtz") == 0) op = 0x07;
    else return -1;

    if (two_reg) {
        NEED(nops == 3 && is_reg(&ops[0]) && is_reg(&ops[1]));
    } else {
        NEED(nops == 2 && is_reg(&ops[0]));
    }

    if (emit) {
        struct mips_operand *t = two_reg ? &ops[2] : &ops[1];
        const char *target = sym_target(t);
        int rt = two_reg ? ops[1].reg : 0;
        int disp;
        NEED(target != NULL);
        disp = branch_disp(a, target, (uint32_t)cur(a)->len);
        emit_word(a, enc_i(op, ops[0].reg, rt, disp));
    }
    return 4;
}

/* jr/jalr (register), j/jal (symbol). */
static int
enc_jump(struct mips_asm *a, const char *m, struct mips_operand *ops,
         int nops, int emit)
{
    if (strcmp(m, "jr") == 0) {
        NEED(nops == 1 && is_reg(&ops[0]));
        if (emit) emit_word(a, enc_r(0, ops[0].reg, 0, 0, 0, 0x08));
        return 4;
    }
    if (strcmp(m, "jalr") == 0) {
        /* jalr $s  (rd = $ra), or jalr $d, $s */
        NEED(nops >= 1 && is_reg(&ops[0]));
        if (nops == 1) {
            if (emit) emit_word(a, enc_r(0, ops[0].reg, 0, 31, 0, 0x09));
        } else {
            NEED(nops == 2 && is_reg(&ops[1]));
            if (emit) emit_word(a, enc_r(0, ops[1].reg, 0, ops[0].reg, 0, 0x09));
        }
        return 4;
    }
    if (strcmp(m, "j") == 0 || strcmp(m, "jal") == 0) {
        int op = strcmp(m, "j") == 0 ? 0x02 : 0x03;
        NEED(nops == 1);
        if (emit) {
            const char *target = sym_target(&ops[0]);
            uint32_t t;
            NEED(target != NULL);
            t = jump_target(a, target, (uint32_t)cur(a)->len);
            emit_word(a, enc_j(op, t));
        }
        return 4;
    }
    return -1;
}

/****************************************************************
 * COP1 (floating point) encoders
 ****************************************************************/

/* fmt field values in the rs position of a COP1 FP op. */
enum { FMT_S = 0x10, FMT_D = 0x11, FMT_W = 0x14 };

/* Split "op.fmt" or "cvt.to.from" style suffixes.  Returns the fmt code for a
 * one-suffix mnemonic like add.d, or -1. */
static int
fmt_of(char c)
{
    if (c == 's') return FMT_S;
    if (c == 'd') return FMT_D;
    if (c == 'w') return FMT_W;
    return -1;
}

/* Moves between GPR and COP1: mfc1/mtc1 name a float register as the second
 * operand; cfc1/ctc1 name a coprocessor control register, which the backend
 * writes as a plain numbered register ($31 = FCSR).  In every case the second
 * operand's number goes in the rd field. */
static int
enc_cop1_move(const char *m, struct mips_operand *ops, int nops, uint32_t *w)
{
    int sub = -1, ctrl = 0;

    if (strcmp(m, "mfc1") == 0) sub = 0x00;
    else if (strcmp(m, "cfc1") == 0) { sub = 0x02; ctrl = 1; }
    else if (strcmp(m, "mtc1") == 0) sub = 0x04;
    else if (strcmp(m, "ctc1") == 0) { sub = 0x06; ctrl = 1; }
    else return -1;

    NEED(nops == 2 && is_reg(&ops[0]));
    NEED(ctrl ? (is_reg(&ops[1]) || is_freg(&ops[1])) : is_freg(&ops[1]));

    /* rs = sub, rt = gpr, rd = fpr/control register */
    *w = enc_r(0x11, sub, ops[0].reg, ops[1].reg, 0, 0);
    return 4;
}

/* bc1f/bc1t: single condition code 0, 16-bit branch offset. */
static int
enc_cop1_branch(struct mips_asm *a, const char *m, struct mips_operand *ops,
                int nops, int emit)
{
    int tf;

    if (strcmp(m, "bc1f") == 0) tf = 0;
    else if (strcmp(m, "bc1t") == 0) tf = 1;
    else return -1;

    NEED(nops == 1);
    if (emit) {
        const char *target = sym_target(&ops[0]);
        int disp;
        NEED(target != NULL);
        disp = branch_disp(a, target, (uint32_t)cur(a)->len);
        emit_word(a, enc_i(0x11, 0x08, tf, disp));
    }
    return 4;
}

/* FP arithmetic (add/sub/mul/div/mov/neg/abs.fmt): fd, fs, ft (ft unused for
 * unary), and the conversions cvt.to.from.  Also the compares c.cond.fmt. */
static int
enc_cop1_fp(const char *m, struct mips_operand *ops, int nops, uint32_t *w)
{
    size_t len = strlen(m);

    /* Conversions cvt.to.from: "cvt." then two single-char format suffixes. */
    if (strncmp(m, "cvt.", 4) == 0 && len == 7 && m[5] == '.') {
        int to = fmt_of(m[4]);
        int from = fmt_of(m[6]);
        int funct;
        NEED(nops == 2 && is_freg(&ops[0]) && is_freg(&ops[1]));
        if (to < 0 || from < 0)
            return -1;
        if (to == FMT_S) funct = 0x20;
        else if (to == FMT_D) funct = 0x21;
        else funct = 0x24;              /* to W (convert to fixed) */
        *w = enc_r(0x11, from, 0, ops[1].reg, ops[0].reg, funct);
        return 4;
    }

    /* Binary/unary arithmetic and compares: mnemonic ends ".s" or ".d". */
    if (len >= 3 && m[len - 2] == '.') {
        int fmt = fmt_of(m[len - 1]);
        char base[8];
        int funct = -1, unary = 0;
        if (fmt < 0 || len - 2 >= sizeof(base))
            return -1;
        memcpy(base, m, len - 2);
        base[len - 2] = '\0';

        if (strcmp(base, "add") == 0) funct = 0x00;
        else if (strcmp(base, "sub") == 0) funct = 0x01;
        else if (strcmp(base, "mul") == 0) funct = 0x02;
        else if (strcmp(base, "div") == 0) funct = 0x03;
        else if (strcmp(base, "abs") == 0) { funct = 0x05; unary = 1; }
        else if (strcmp(base, "mov") == 0) { funct = 0x06; unary = 1; }
        else if (strcmp(base, "neg") == 0) { funct = 0x07; unary = 1; }

        if (funct >= 0) {
            if (unary) {
                NEED(nops == 2 && is_freg(&ops[0]) && is_freg(&ops[1]));
                *w = enc_r(0x11, fmt, 0, ops[1].reg, ops[0].reg, funct);
            } else {
                NEED(nops == 3 && is_freg(&ops[0]) && is_freg(&ops[1])
                     && is_freg(&ops[2]));
                /* fmt, ft=ops[2], fs=ops[1], fd=ops[0] */
                *w = enc_r(0x11, fmt, ops[2].reg, ops[1].reg, ops[0].reg, funct);
            }
            return 4;
        }

        /* Compare c.cond.fmt: fs, ft; sets condition code 0. */
        if (m[0] == 'c' && m[1] == '.') {
            int cond = -1;
            if (strncmp(base + 2, "eq", 2) == 0 && base[4] == '\0') cond = 2;
            else if (strncmp(base + 2, "lt", 2) == 0 && base[4] == '\0') cond = 12;
            else if (strncmp(base + 2, "le", 2) == 0 && base[4] == '\0') cond = 14;
            if (cond >= 0) {
                NEED(nops == 2 && is_freg(&ops[0]) && is_freg(&ops[1]));
                /* fmt, ft=ops[1], fs=ops[0], funct = 0x30 | cond */
                *w = enc_r(0x11, fmt, ops[1].reg, ops[0].reg, 0, 0x30 | cond);
                return 4;
            }
        }
        return -1;
    }

    return -1;
}

/****************************************************************
 * Macro expansion
 *
 * The macros the backend leans on, expanded to match GNU as byte for byte.
 * Each returns the total encoded size (emitting the sequence when emit is set)
 * or -1 if the mnemonic or operand shape is not this macro.
 ****************************************************************/

#define AT 1        /* $at, the assembler temporary the macros clobber */

/* li $d, imm : the shortest of addiu / ori / lui / lui+ori that GNU picks. */
static int
enc_li(struct mips_asm *a, struct mips_operand *ops, int nops, int emit)
{
    int32_t v;

    NEED(nops == 2 && is_reg(&ops[0]) && is_imm(&ops[1]));
    v = (int32_t)ops[1].imm;

    if (v >= -32768 && v <= 32767) {
        if (emit) emit_word(a, enc_i(0x09, 0, ops[0].reg, v));   /* addiu */
        return 4;
    }
    if (v >= 0 && v <= 65535) {
        if (emit) emit_word(a, enc_i(0x0d, 0, ops[0].reg, v));   /* ori */
        return 4;
    }
    if (((uint32_t)v & 0xffff) == 0) {
        if (emit) emit_word(a, enc_i(0x0f, 0, ops[0].reg,
            (int)(((uint32_t)v >> 16) & 0xffff)));               /* lui */
        return 4;
    }
    if (emit) {
        emit_word(a, enc_i(0x0f, 0, ops[0].reg,
            (int)(((uint32_t)v >> 16) & 0xffff)));               /* lui */
        emit_word(a, enc_i(0x0d, ops[0].reg, ops[0].reg,
            (int)((uint32_t)v & 0xffff)));                       /* ori */
    }
    return 8;
}

/* The signed div/rem trap: divide-by-zero break, then the INT_MIN/-1 overflow
 * break; the unsigned form omits the overflow check.  hilo picks mflo (div) or
 * mfhi (rem). */
static void
emit_div_trap(struct mips_asm *a, int s, int t, int d, int is_signed,
              int funct_div, int hilo)
{
    emit_word(a, enc_i(0x05, t, 0, 2));             /* bne $t,$zero,+2 */
    emit_word(a, enc_r(0, s, t, 0, 0, funct_div));  /* div/divu $zero,$s,$t */
    emit_word(a, 0x0007000d);                       /* break 7 */
    if (is_signed) {
        emit_word(a, enc_i(0x09, 0, AT, -1));       /* li $at,-1 */
        emit_word(a, enc_i(0x05, t, AT, 4));        /* bne $t,$at,+4 */
        emit_word(a, enc_i(0x0f, 0, AT, 0x8000));   /* lui $at,0x8000 */
        emit_word(a, enc_i(0x05, s, AT, 2));        /* bne $s,$at,+2 */
        emit_word(a, 0);                            /* nop */
        emit_word(a, 0x0006000d);                   /* break 6 */
    }
    emit_word(a, enc_r(0, 0, 0, d, 0, hilo));       /* mflo/mfhi $d */
}

/* Macro dispatch.  Returns the encoded size, or -1 if m is not a macro (or the
 * operand shape does not match this macro, so a real instruction is tried). */
static int
enc_macro(struct mips_asm *a, const char *m, struct mips_operand *ops,
          int nops, int emit)
{
    if (strcmp(m, "move") == 0) {
        NEED(nops == 2 && is_reg(&ops[0]) && is_reg(&ops[1]));
        if (emit) emit_word(a, enc_r(0, ops[1].reg, 0, ops[0].reg, 0, 0x25));
        return 4;
    }
    if (strcmp(m, "negu") == 0 || strcmp(m, "neg") == 0) {
        int funct = strcmp(m, "negu") == 0 ? 0x23 : 0x22;
        NEED(nops == 2 && is_reg(&ops[0]) && is_reg(&ops[1]));
        if (emit) emit_word(a, enc_r(0, 0, ops[1].reg, ops[0].reg, 0, funct));
        return 4;
    }
    if (strcmp(m, "not") == 0) {                    /* nor $d, $s, $zero */
        NEED(nops == 2 && is_reg(&ops[0]) && is_reg(&ops[1]));
        if (emit) emit_word(a, enc_r(0, ops[1].reg, 0, ops[0].reg, 0, 0x27));
        return 4;
    }
    if (strcmp(m, "li") == 0)
        return enc_li(a, ops, nops, emit);

    if (strcmp(m, "la") == 0) {
        const char *sym;
        NEED(nops == 2 && is_reg(&ops[0]));
        sym = sym_target(&ops[1]);
        NEED(sym != NULL);
        if (emit) {
            reloc_sym(a, (uint32_t)cur(a)->len, sym, 0, R_MIPS_HI16);
            emit_word(a, enc_i(0x0f, 0, ops[0].reg, 0));          /* lui */
            reloc_sym(a, (uint32_t)cur(a)->len, sym, 0, R_MIPS_LO16);
            emit_word(a, enc_i(0x09, ops[0].reg, ops[0].reg, 0)); /* addiu */
        }
        return 8;
    }

    if (strcmp(m, "beqz") == 0 || strcmp(m, "bnez") == 0
        || strcmp(m, "b") == 0) {
        int op = strcmp(m, "bnez") == 0 ? 0x05 : 0x04;
        int rs = 0, argi = 0;
        if (strcmp(m, "b") != 0) {
            NEED(nops == 2 && is_reg(&ops[0]));
            rs = ops[0].reg;
            argi = 1;
        } else {
            NEED(nops == 1);
        }
        if (emit) {
            const char *target = sym_target(&ops[argi]);
            int disp;
            NEED(target != NULL);
            disp = branch_disp(a, target, (uint32_t)cur(a)->len);
            emit_word(a, enc_i(op, rs, 0, disp));
        }
        return 4;
    }

    if (strcmp(m, "l.d") == 0 || strcmp(m, "s.d") == 0
        || strcmp(m, "l.s") == 0 || strcmp(m, "s.s") == 0) {
        int store = m[0] == 's';
        int dbl = m[2] == 'd';
        int op = store ? 0x39 : 0x31;               /* swc1 / lwc1 */
        NEED(nops == 2 && is_freg(&ops[0]) && is_mem(&ops[1]));
        if (emit) {
            emit_word(a, enc_i(op, ops[1].reg, ops[0].reg, (int)ops[1].imm));
            if (dbl)
                emit_word(a, enc_i(op, ops[1].reg, ops[0].reg + 1,
                    (int)ops[1].imm + 4));
        }
        return dbl ? 8 : 4;
    }

    if (strcmp(m, "mul") == 0) {
        NEED(nops == 3 && is_reg(&ops[0]) && is_reg(&ops[1]) && is_reg(&ops[2]));
        if (emit) {
            emit_word(a, enc_r(0, ops[1].reg, ops[2].reg, 0, 0, 0x19)); /* multu */
            emit_word(a, enc_r(0, 0, 0, ops[0].reg, 0, 0x12));         /* mflo */
        }
        return 8;
    }

    if (strcmp(m, "div") == 0 || strcmp(m, "divu") == 0
        || strcmp(m, "rem") == 0 || strcmp(m, "remu") == 0) {
        int unsgn = (m[strlen(m) - 1] == 'u');
        int is_rem = (m[0] == 'r');
        int funct_div = unsgn ? 0x1b : 0x1a;
        int hilo = is_rem ? 0x10 : 0x12;            /* mfhi : mflo */
        NEED(nops == 3 && is_reg(&ops[0]) && is_reg(&ops[1]) && is_reg(&ops[2]));
        /* div/divu with a $zero destination is the raw instruction. */
        if (ops[0].reg == 0 && !is_rem) {
            if (emit)
                emit_word(a, enc_r(0, ops[1].reg, ops[2].reg, 0, 0, funct_div));
            return 4;
        }
        if (emit)
            emit_div_trap(a, ops[1].reg, ops[2].reg, ops[0].reg,
                !unsgn, funct_div, hilo);
        return unsgn ? 16 : 40;
    }

    return -1;
}

/****************************************************************
 * Dispatch
 ****************************************************************/

int
mips_encode(struct mips_asm *a, const char *m, struct mips_operand *ops,
            int nops, int emit)
{
    uint32_t w = 0;
    int r;

    /* nop is sll $0, $0, 0 */
    if (strcmp(m, "nop") == 0) {
        if (emit) emit_word(a, 0);
        return 4;
    }

    /* syscall / break: SPECIAL, funct in bits 5-0.  A single code operand goes
     * in bits 25-6 for syscall, and in bits 25-16 for break (matching GNU as,
     * which reads break's argument as the upper code field). */
    if (strcmp(m, "syscall") == 0 || strcmp(m, "break") == 0) {
        int is_break = strcmp(m, "break") == 0;
        int funct = is_break ? 0x0d : 0x0c;
        uint32_t code = (nops == 1 && is_imm(&ops[0]))
            ? (uint32_t)ops[0].imm : 0;
        if (emit)
            emit_word(a, (uint32_t)(funct & 63)
                | (is_break ? (code & 0x3ff) << 16 : (code & 0xfffff) << 6));
        return 4;
    }

    /* Macros expand to one or more real instructions.  Tried before the base
     * encoders: a 3-operand div is this macro, a 2-operand div is the raw
     * instruction below, disambiguated by operand count inside enc_macro. */
    if ((r = enc_macro(a, m, ops, nops, emit)) >= 0)
        return r;

    /* Word-producing groups: encode into w, emit if requested. */
    if ((r = enc_rrr(m, ops, nops, &w)) >= 0
        || (r = enc_shiftv(m, ops, nops, &w)) >= 0
        || (r = enc_shifti(m, ops, nops, &w)) >= 0
        || (r = enc_muldiv(m, ops, nops, &w)) >= 0
        || (r = enc_hilo(m, ops, nops, &w)) >= 0
        || (r = enc_cop1_move(m, ops, nops, &w)) >= 0
        || (r = enc_cop1_fp(m, ops, nops, &w)) >= 0) {
        if (emit) emit_word(a, w);
        return r;
    }

    /* Groups that emit directly (relocations, PC-relative fields). */
    if ((r = enc_arithi(a, m, ops, nops, emit)) >= 0) return r;
    if ((r = enc_lui(a, ops, nops, emit)) >= 0) return r;
    if ((r = enc_memop(a, m, ops, nops, emit)) >= 0) return r;
    if ((r = enc_branch(a, m, ops, nops, emit)) >= 0) return r;
    if ((r = enc_jump(a, m, ops, nops, emit)) >= 0) return r;
    if ((r = enc_cop1_branch(a, m, ops, nops, emit)) >= 0) return r;

    return -1;
}
