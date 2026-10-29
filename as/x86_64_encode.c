/* x86_64_encode.c : the x86-64 instruction encoder for skj-as-x86-64.
 *
 * One entry point, x86_64_encode, turns a parsed mnemonic plus operands into
 * bytes in the current section.  The byte recipes (REX, ModRM, SIB, the SSE
 * prefix order and opcode map) mirror the JIT encoder jit/emit_x86.c, which is
 * checked byte-for-byte against GNU as, so this assembler agrees with both.
 *
 * x86 instruction length is a function of the operand values alone, so the two
 * passes emit the same bytes; only relocations and resolved branch targets are
 * withheld until pass 2.  Branches always use the rel32 form (no relaxation): a
 * branch to a symbol defined in the current section resolves to a direct
 * displacement, any other target becomes a PC32/PLT32 relocation.
 */

#include <stdlib.h>
#include <string.h>

#include "x86_64.h"
#include "util.h"

/****************************************************************
 * Byte emission and ModRM / REX / SIB
 ****************************************************************/

static void e8(struct section *s, uint8_t v)  { sec_emit8(s, v); }
static void e32(struct section *s, uint32_t v) { sec_emit32(s, v); }
static void e64(struct section *s, uint64_t v)
{
    sec_emit32(s, (uint32_t)(v & 0xffffffffu));
    sec_emit32(s, (uint32_t)(v >> 32));
}

/* A byte register spl/bpl/sil/dil (hardware 4..7) forces a REX so the low byte
   is addressed rather than ah/ch/dh/bh. */
static int
byte_needs_rex(const struct x64_operand *o)
{
    return o->kind == XO_REG && o->rclass == RC_GPR && o->size == 1 &&
           o->reg >= 4 && o->reg <= 7;
}

static void
rex(struct section *s, int w, int reg, int idx, int rm, int force)
{
    int r = (reg >> 3) & 1;
    int x = (idx >> 3) & 1;
    int b = (rm >> 3) & 1;
    if (w || r || x || b || force)
        e8(s, (uint8_t)(0x40 | (w << 3) | (r << 2) | (x << 1) | b));
}

static void
modrm_rr(struct section *s, int reg, int rm)
{
    e8(s, (uint8_t)(0xC0 | ((reg & 7) << 3) | (rm & 7)));
}

/* Resolve a branch / RIP-relative reference: a symbol defined in the current
   section becomes a direct displacement; anything else a PC-relative
   relocation (type PC32 for a data address, PLT32 for a call/jmp target). */
static void
emit_rel32(struct x86_64_asm *a, struct section *s, const char *name,
           int32_t add, int type, int emit)
{
    uint32_t fieldoff = (uint32_t)s->len;

    if (!name)
        die("x86-64: branch or rip-relative operand is not a symbol");
    if (!emit) {
        e32(s, 0);
        return;
    }
    int idx = sym_lookup(&a->st, name);
    /* A symbol defined in this section resolves to a direct displacement, but
       only when it is local: a global (or weak) symbol keeps its relocation so
       the linker can honour interposition and visibility, matching GNU as. */
    if (idx >= 0 && a->st.syms[idx].defined && !a->st.syms[idx].global &&
        !a->st.syms[idx].weak && a->st.syms[idx].section == a->cur_section) {
        long target = (long)a->st.syms[idx].value + add;
        e32(s, (uint32_t)(int32_t)(target - (long)(fieldoff + 4)));
        return;
    }
    if (idx < 0)
        idx = sym_add(&a->st, name);
    e32(s, 0);
    sec_add_reloc_t(s, fieldoff, idx, add - 4, type);
}

/* An absolute reference (offset sym, [sym], .long/.quad sym): the address is a
   link-time value, so it is always a relocation, never resolved here. */
static void
emit_abs(struct x86_64_asm *a, struct section *s, const char *name,
         int32_t add, int type, int width, int emit)
{
    uint32_t fieldoff = (uint32_t)s->len;

    if (width == 8)
        e64(s, 0);
    else
        e32(s, 0);
    if (emit) {
        int idx = sym_lookup(&a->st, name);
        if (idx < 0)
            idx = sym_add(&a->st, name);
        sec_add_reloc_t(s, fieldoff, idx, add, type);
    }
}

/* Emit the ModRM (+ SIB + displacement + any relocation) for a memory operand,
   with `regf` in the reg field.  REX must already have been emitted. */
static void
emit_modrm_mem(struct x86_64_asm *a, struct section *s, int regf,
               struct x64_operand *m, int emit)
{
    if (m->rip) {                               /* [rip + sym] */
        e8(s, (uint8_t)(((regf & 7) << 3) | 0x05));
        emit_rel32(a, s, m->sym, m->add, R_X86_64_PC32, emit);
        return;
    }
    if (m->base < 0) {                          /* [sym] absolute (SIB, no base) */
        e8(s, (uint8_t)(((regf & 7) << 3) | 0x04));
        e8(s, 0x25);                            /* scale 0, index none, base none */
        if (m->is_sym)
            emit_abs(a, s, m->sym, m->add, R_X86_64_32S, 4, emit);
        else
            e32(s, (uint32_t)m->disp);
        return;
    }
    {
        int bb = m->base & 7;
        int has_index = (m->index >= 0);
        long disp = m->disp;
        int mod;

        if (disp == 0 && bb != 5)               /* rbp/r13 need an explicit disp */
            mod = 0;
        else if (disp >= -128 && disp <= 127)
            mod = 1;
        else
            mod = 2;

        if (has_index || bb == 4) {             /* SIB: index present, or rsp/r12 base */
            int ss = m->scale == 8 ? 3 : m->scale == 4 ? 2 : m->scale == 2 ? 1 : 0;
            int ireg = has_index ? (m->index & 7) : 4;   /* 4 = no index */
            e8(s, (uint8_t)((mod << 6) | ((regf & 7) << 3) | 0x04));
            e8(s, (uint8_t)((ss << 6) | (ireg << 3) | bb));
        } else {
            e8(s, (uint8_t)((mod << 6) | ((regf & 7) << 3) | bb));
        }
        if (mod == 1)
            e8(s, (uint8_t)disp);
        else if (mod == 2)
            e32(s, (uint32_t)disp);
    }
}

/* REX for an instruction whose r/m is a memory operand: X from the index, B
   from the base (both absent for rip / absolute). */
static void
rex_mem(struct section *s, int w, int regf, struct x64_operand *m, int force)
{
    int idx = (m->index >= 0) ? m->index : 0;
    int base = (m->base >= 0) ? m->base : 0;
    rex(s, w, regf, idx, base, force);
}

/****************************************************************
 * Operand-size helpers
 ****************************************************************/

static int reg_is(const struct x64_operand *o, int kind) { return o->kind == kind; }

/****************************************************************
 * Encoders by shape
 ****************************************************************/

/* ALU "op r/m, r" (01/09/.../39) and "op r, r/m" plus the immediate and memory
   forms, selected by operand kinds.  `op_mr` is the r/m,r opcode; `ext` is the
   /digit for the immediate (group-1) form. */
static void
enc_alu(struct x86_64_asm *a, struct section *s, uint8_t op_mr, int ext,
        struct x64_operand *ops, int nops, int emit)
{
    struct x64_operand *d = &ops[0], *srco = &ops[1];
    int w, force;

    (void)nops;
    if (reg_is(d, XO_REG) && reg_is(srco, XO_REG)) {    /* op r/m, r */
        int size = d->size;
        w = size == 8;
        force = byte_needs_rex(d) || byte_needs_rex(srco);
        if (size == 2) e8(s, 0x66);
        rex(s, w, srco->reg, 0, d->reg, force);
        e8(s, size == 1 ? (uint8_t)(op_mr - 1) : op_mr);
        modrm_rr(s, srco->reg, d->reg);
        return;
    }
    if (reg_is(d, XO_REG) && reg_is(srco, XO_IMM)) {    /* op r, imm */
        int size = d->size;
        long v = srco->disp;
        w = size == 8;
        if (size == 2) e8(s, 0x66);
        force = byte_needs_rex(d);
        if (!srco->is_sym && !srco->wide_imm && v >= -128 && v <= 127 && size != 1) {
            rex(s, w, 0, 0, d->reg, force);     /* group-1 r/m, imm8 */
            e8(s, 0x83);
            modrm_rr(s, ext, d->reg);
            e8(s, (uint8_t)v);
        } else if (!srco->is_sym && d->reg == 0) {
            /* accumulator short form: op al/ax/eax/rax, imm (what GNU as picks) */
            rex(s, w, 0, 0, 0, 0);
            if (size == 1)      { e8(s, (uint8_t)(op_mr + 3)); e8(s, (uint8_t)v); }
            else if (size == 2) { e8(s, (uint8_t)(op_mr + 4)); sec_emit16(s, (uint16_t)v); }
            else                { e8(s, (uint8_t)(op_mr + 4)); e32(s, (uint32_t)v); }
        } else {
            rex(s, w, 0, 0, d->reg, force);
            e8(s, size == 1 ? 0x80 : 0x81);
            modrm_rr(s, ext, d->reg);
            if (size == 1)      e8(s, (uint8_t)v);
            else if (size == 2) sec_emit16(s, (uint16_t)v);
            else                e32(s, (uint32_t)v);
        }
        return;
    }
    if (reg_is(d, XO_MEM) && reg_is(srco, XO_REG)) {    /* op [mem], r */
        int size = srco->size;
        w = size == 8;
        if (size == 2) e8(s, 0x66);
        rex_mem(s, w, srco->reg, d, byte_needs_rex(srco));
        e8(s, size == 1 ? (uint8_t)(op_mr - 1) : op_mr);
        emit_modrm_mem(a, s, srco->reg, d, emit);
        return;
    }
    if (reg_is(d, XO_REG) && reg_is(srco, XO_MEM)) {    /* op r, [mem] */
        int size = d->size;
        w = size == 8;
        if (size == 2) e8(s, 0x66);
        rex_mem(s, w, d->reg, srco, byte_needs_rex(d));
        e8(s, size == 1 ? (uint8_t)(op_mr + 1) : (uint8_t)(op_mr + 2)); /* r, r/m */
        emit_modrm_mem(a, s, d->reg, srco, emit);
        return;
    }
    die("x86-64: unsupported operand form");
}

/* mov in all its shapes. */
static void
enc_mov(struct x86_64_asm *a, struct section *s,
        struct x64_operand *ops, int nops, int emit)
{
    struct x64_operand *d = &ops[0], *srco = &ops[1];
    (void)nops;

    if (reg_is(d, XO_REG) && reg_is(srco, XO_REG)) {        /* mov r/m, r */
        int size = d->size, w = size == 8;
        int force = byte_needs_rex(d) || byte_needs_rex(srco);
        if (size == 2) e8(s, 0x66);
        rex(s, w, srco->reg, 0, d->reg, force);
        e8(s, size == 1 ? 0x88 : 0x89);
        modrm_rr(s, srco->reg, d->reg);
        return;
    }
    if (reg_is(d, XO_REG) && reg_is(srco, XO_IMM)) {        /* mov r, imm / offset sym */
        int size = d->size;
        if (srco->is_sym) {                                 /* address immediate */
            if (size == 8) {                                /* C7 /0 imm32 (sign-ext) */
                rex(s, 1, 0, 0, d->reg, 0);
                e8(s, 0xC7);
                modrm_rr(s, 0, d->reg);
                emit_abs(a, s, srco->sym, srco->add, R_X86_64_32S, 4, emit);
            } else {                                        /* B8+r imm32 (zero-ext) */
                rex(s, 0, 0, 0, d->reg, 0);
                e8(s, (uint8_t)(0xB8 + (d->reg & 7)));
                emit_abs(a, s, srco->sym, srco->add, R_X86_64_32, 4, emit);
            }
            return;
        }
        if (size == 8) {
            long v = srco->disp;
            if (srco->wide_imm) {                           /* folded label diff */
                if (v < -2147483647L - 1 || v > 2147483647L)
                    die("x86-64: expression does not fit a 32-bit immediate");
                rex(s, 1, 0, 0, d->reg, 0);
                e8(s, 0xC7);
                modrm_rr(s, 0, d->reg);
                e32(s, (uint32_t)(int32_t)v);
            } else if (v >= -2147483647L - 1 && v <= 2147483647L) { /* imm32: C7 /0 */
                rex(s, 1, 0, 0, d->reg, 0);
                e8(s, 0xC7);
                modrm_rr(s, 0, d->reg);
                e32(s, (uint32_t)(int32_t)v);
            } else {                                        /* movabs imm64 */
                rex(s, 1, 0, 0, d->reg, 0);
                e8(s, (uint8_t)(0xB8 + (d->reg & 7)));
                e64(s, (uint64_t)v);
            }
            return;
        }
        if (size == 2) e8(s, 0x66);
        if (size == 1) {
            rex(s, 0, 0, 0, d->reg, byte_needs_rex(d));
            e8(s, (uint8_t)(0xB0 + (d->reg & 7)));
            e8(s, (uint8_t)srco->disp);
        } else {
            rex(s, 0, 0, 0, d->reg, 0);
            e8(s, (uint8_t)(0xB8 + (d->reg & 7)));
            if (size == 2) e32(s, (uint32_t)srco->disp); /* imm16 handled by writer width */
            else           e32(s, (uint32_t)srco->disp);
        }
        return;
    }
    if (reg_is(d, XO_REG) && reg_is(srco, XO_MEM)) {        /* mov r, [mem] */
        int size = d->size, w = size == 8;
        if (size == 2) e8(s, 0x66);
        rex_mem(s, w, d->reg, srco, 0);
        e8(s, size == 1 ? 0x8A : 0x8B);
        emit_modrm_mem(a, s, d->reg, srco, emit);
        return;
    }
    if (reg_is(d, XO_MEM) && reg_is(srco, XO_REG)) {        /* mov [mem], r */
        int size = srco->size, w = size == 8;
        if (size == 2) e8(s, 0x66);
        rex_mem(s, w, srco->reg, d, byte_needs_rex(srco));
        e8(s, size == 1 ? 0x88 : 0x89);
        emit_modrm_mem(a, s, srco->reg, d, emit);
        return;
    }
    if (reg_is(d, XO_MEM) && reg_is(srco, XO_IMM)) {        /* mov [mem], imm / offset sym */
        int size = d->mem_size ? d->mem_size : 4;
        int w = size == 8;
        if (size == 2) e8(s, 0x66);
        rex_mem(s, w, 0, d, 0);
        e8(s, size == 1 ? 0xC6 : 0xC7);
        emit_modrm_mem(a, s, 0, d, emit);
        if (srco->is_sym)
            emit_abs(a, s, srco->sym, srco->add,
                     size == 8 ? R_X86_64_32S : R_X86_64_32, 4, emit);
        else if (size == 1)
            e8(s, (uint8_t)srco->disp);
        else
            e32(s, (uint32_t)srco->disp);
        return;
    }
    die("x86-64: unsupported mov form");
}

/* movzx / movsx: 0F B6/B7 (zero) or BE/BF (sign), reg = gpr dest, r/m = r8/r16
   or a byte/word memory operand. */
static void
enc_movx(struct x86_64_asm *a, struct section *s, int sign,
         struct x64_operand *ops, int emit)
{
    struct x64_operand *d = &ops[0], *srco = &ops[1];
    int w = d->size == 8;
    int srcsize = srco->kind == XO_MEM ?
                  (srco->mem_size ? srco->mem_size : 1) : srco->size;
    uint8_t op = (uint8_t)((sign ? 0xBE : 0xB6) + (srcsize == 2 ? 1 : 0));

    if (srco->kind == XO_REG) {
        rex(s, w, d->reg, 0, srco->reg, byte_needs_rex(srco));
        e8(s, 0x0F);
        e8(s, op);
        modrm_rr(s, d->reg, srco->reg);
    } else {
        rex_mem(s, w, d->reg, srco, 0);
        e8(s, 0x0F);
        e8(s, op);
        emit_modrm_mem(a, s, d->reg, srco, emit);
    }
}

/* F7 /ext group: neg(/3) not(/2) idiv(/7) div(/6). */
static void
enc_f7(struct section *s, int ext, struct x64_operand *o)
{
    int w = o->size == 8;
    if (o->size == 2) e8(s, 0x66);
    rex(s, w, 0, 0, o->reg, byte_needs_rex(o));
    e8(s, o->size == 1 ? 0xF6 : 0xF7);
    modrm_rr(s, ext, o->reg);
}

/* Shift group shl(/4) shr(/5) sar(/7): by cl (D3), by 1 (D1), or by imm8 (C1),
   and the 8-bit D2/D0/C0 variants. */
static void
enc_shift(struct section *s, int ext, struct x64_operand *ops)
{
    struct x64_operand *d = &ops[0], *cnt = &ops[1];
    int size = d->size, w = size == 8;

    if (size == 2) e8(s, 0x66);
    rex(s, w, 0, 0, d->reg, byte_needs_rex(d));
    if (cnt->kind == XO_REG) {                      /* shift r/m, cl */
        e8(s, size == 1 ? 0xD2 : 0xD3);
        modrm_rr(s, ext, d->reg);
    } else if (cnt->kind == XO_IMM && !cnt->is_sym && cnt->disp == 1) {
        e8(s, size == 1 ? 0xD0 : 0xD1);             /* shift r/m, 1 */
        modrm_rr(s, ext, d->reg);
    } else {                                        /* shift r/m, imm8 */
        e8(s, size == 1 ? 0xC0 : 0xC1);
        modrm_rr(s, ext, d->reg);
        e8(s, (uint8_t)cnt->disp);
    }
}

/* An SSE op: mandatory prefix (0 = none), then REX, then 0F + opcode.  reg and
   r/m are both xmm, or reg xmm and r/m a gpr / memory. */
static void
enc_sse(struct x86_64_asm *a, struct section *s, uint8_t pfx, uint8_t op, int w,
        struct x64_operand *reg, struct x64_operand *rm, int emit)
{
    if (pfx) e8(s, pfx);
    if (rm->kind == XO_MEM) {
        rex_mem(s, w, reg->reg, rm, 0);
        e8(s, 0x0F);
        e8(s, op);
        emit_modrm_mem(a, s, reg->reg, rm, emit);
    } else {
        rex(s, w, reg->reg, 0, rm->reg, 0);
        e8(s, 0x0F);
        e8(s, op);
        modrm_rr(s, reg->reg, rm->reg);
    }
}

/****************************************************************
 * Dispatch
 ****************************************************************/

struct sse_form { const char *name; uint8_t pfx; uint8_t op; int w; int rm_is_gpr; };

static const struct sse_form sse_table[] = {
    { "movsd",     0xF2, 0x10, 0, 0 },   /* reg,rm (load/rr); store handled below */
    { "addsd",     0xF2, 0x58, 0, 0 },
    { "subsd",     0xF2, 0x5C, 0, 0 },
    { "mulsd",     0xF2, 0x59, 0, 0 },
    { "divsd",     0xF2, 0x5E, 0, 0 },
    { "sqrtsd",    0xF2, 0x51, 0, 0 },
    { "ucomisd",   0x66, 0x2E, 0, 0 },
    { "movss",     0xF3, 0x10, 0, 0 },
    { "addss",     0xF3, 0x58, 0, 0 },
    { "subss",     0xF3, 0x5C, 0, 0 },
    { "mulss",     0xF3, 0x59, 0, 0 },
    { "divss",     0xF3, 0x5E, 0, 0 },
    { "ucomiss",   0x00, 0x2E, 0, 0 },
    { "cvtss2sd",  0xF3, 0x5A, 0, 0 },
    { "cvtsd2ss",  0xF2, 0x5A, 0, 0 },
    { "cvtsi2sd",  0xF2, 0x2A, 0, 1 },   /* xmm, r32 */
    { "cvtsi2ss",  0xF3, 0x2A, 0, 1 },
    { "cvttsd2si", 0xF2, 0x2C, 0, 1 },   /* r32, xmm (reg=gpr) */
    { "cvttss2si", 0xF3, 0x2C, 0, 1 },
    { "cvtsd2si",  0xF2, 0x2D, 0, 1 },
    { "xorps",     0x00, 0x57, 0, 0 },
    { "xorpd",     0x66, 0x57, 0, 0 },
    { "andps",     0x00, 0x54, 0, 0 },
    { "andpd",     0x66, 0x54, 0, 0 },
    { NULL, 0, 0, 0, 0 },
};

void
x86_64_encode(struct x86_64_asm *a, const char *mnem,
              struct x64_operand *ops, int nops, int emit)
{
    struct section *s = &a->sections[a->cur_section];
    int i;

    /* zero-operand and simple forms first */
    if (!strcmp(mnem, "ret"))   { e8(s, 0xC3); return; }
    if (!strcmp(mnem, "leave")) { e8(s, 0xC9); return; }
    if (!strcmp(mnem, "nop"))   { e8(s, 0x90); return; }
    if (!strcmp(mnem, "cqo"))   { e8(s, 0x48); e8(s, 0x99); return; }
    if (!strcmp(mnem, "cdq"))   { e8(s, 0x99); return; }
    if (!strcmp(mnem, "syscall")){ e8(s, 0x0F); e8(s, 0x05); return; }
    if (!strcmp(mnem, "rep")) {
        /* only `rep movsq` is emitted */
        e8(s, 0xF3); e8(s, 0x48); e8(s, 0xA5); return;
    }

    if (!strcmp(mnem, "mov"))  { enc_mov(a, s, ops, nops, emit); return; }
    if (!strcmp(mnem, "movzx")){ enc_movx(a, s, 0, ops, emit); return; }
    if (!strcmp(mnem, "movsx")){ enc_movx(a, s, 1, ops, emit); return; }
    if (!strcmp(mnem, "movsxd")) {
        if (ops[1].kind == XO_MEM) {
            rex_mem(s, 1, ops[0].reg, &ops[1], 0);
            e8(s, 0x63);
            emit_modrm_mem(a, s, ops[0].reg, &ops[1], emit);
        } else {
            rex(s, 1, ops[0].reg, 0, ops[1].reg, 0);
            e8(s, 0x63);
            modrm_rr(s, ops[0].reg, ops[1].reg);
        }
        return;
    }

    if (!strcmp(mnem, "add")) { enc_alu(a, s, 0x01, 0, ops, nops, emit); return; }
    if (!strcmp(mnem, "or"))  { enc_alu(a, s, 0x09, 1, ops, nops, emit); return; }
    if (!strcmp(mnem, "and")) { enc_alu(a, s, 0x21, 4, ops, nops, emit); return; }
    if (!strcmp(mnem, "sub")) { enc_alu(a, s, 0x29, 5, ops, nops, emit); return; }
    if (!strcmp(mnem, "xor")) { enc_alu(a, s, 0x31, 6, ops, nops, emit); return; }
    if (!strcmp(mnem, "cmp")) { enc_alu(a, s, 0x39, 7, ops, nops, emit); return; }
    if (!strcmp(mnem, "test")) {
        int size = ops[0].size, w = size == 8;
        if (size == 2) e8(s, 0x66);
        rex(s, w, ops[1].reg, 0, ops[0].reg,
            byte_needs_rex(&ops[0]) || byte_needs_rex(&ops[1]));
        e8(s, size == 1 ? 0x84 : 0x85);
        modrm_rr(s, ops[1].reg, ops[0].reg);
        return;
    }

    if (!strcmp(mnem, "imul")) {
        int w = ops[0].size == 8;
        if (ops[1].kind == XO_REG) {
            rex(s, w, ops[0].reg, 0, ops[1].reg, 0);
            e8(s, 0x0F); e8(s, 0xAF);
            modrm_rr(s, ops[0].reg, ops[1].reg);
        } else {
            rex_mem(s, w, ops[0].reg, &ops[1], 0);
            e8(s, 0x0F); e8(s, 0xAF);
            emit_modrm_mem(a, s, ops[0].reg, &ops[1], emit);
        }
        return;
    }
    if (!strcmp(mnem, "neg"))  { enc_f7(s, 3, &ops[0]); return; }
    if (!strcmp(mnem, "not"))  { enc_f7(s, 2, &ops[0]); return; }
    if (!strcmp(mnem, "idiv")) { enc_f7(s, 7, &ops[0]); return; }
    if (!strcmp(mnem, "div"))  { enc_f7(s, 6, &ops[0]); return; }
    if (!strcmp(mnem, "shl") || !strcmp(mnem, "sal")) { enc_shift(s, 4, ops); return; }
    if (!strcmp(mnem, "shr"))  { enc_shift(s, 5, ops); return; }
    if (!strcmp(mnem, "sar"))  { enc_shift(s, 7, ops); return; }

    if (!strcmp(mnem, "push")) {
        rex(s, 0, 0, 0, ops[0].reg, 0);
        e8(s, (uint8_t)(0x50 + (ops[0].reg & 7)));
        return;
    }
    if (!strcmp(mnem, "pop")) {
        rex(s, 0, 0, 0, ops[0].reg, 0);
        e8(s, (uint8_t)(0x58 + (ops[0].reg & 7)));
        return;
    }

    if (!strcmp(mnem, "lea")) {                 /* lea r64, mem (or [rip+sym]) */
        rex_mem(s, 1, ops[0].reg, &ops[1], 0);
        e8(s, 0x8D);
        emit_modrm_mem(a, s, ops[0].reg, &ops[1], emit);
        return;
    }

    if (!strcmp(mnem, "call") || !strcmp(mnem, "jmp")) {
        int isjmp = mnem[0] == 'j';
        int ext = isjmp ? 4 : 2;
        if (ops[0].kind == XO_REG) {            /* call/jmp r64 */
            rex(s, 0, 0, 0, ops[0].reg, 0);
            e8(s, 0xFF);
            modrm_rr(s, ext, ops[0].reg);
        } else if (ops[0].kind == XO_MEM) {     /* call/jmp [mem] */
            rex_mem(s, 0, ext, &ops[0], 0);
            e8(s, 0xFF);
            emit_modrm_mem(a, s, ext, &ops[0], emit);
        } else {                                /* call/jmp rel32 (symbol) */
            e8(s, isjmp ? 0xE9 : 0xE8);
            emit_rel32(a, s, ops[0].sym, ops[0].add, R_X86_64_PLT32, emit);
        }
        return;
    }

    if (mnem[0] == 'j') {                        /* jCC rel32 */
        int cc = x64_cc(mnem + 1);
        if (cc < 0) die("x86-64: unknown branch '%s'", mnem);
        e8(s, 0x0F);
        e8(s, (uint8_t)(0x80 + cc));
        emit_rel32(a, s, ops[0].sym, ops[0].add, R_X86_64_PLT32, emit);
        return;
    }
    if (!strncmp(mnem, "set", 3)) {              /* setCC r/m8 */
        int cc = x64_cc(mnem + 3);
        if (cc < 0) die("x86-64: unknown setcc '%s'", mnem);
        rex(s, 0, 0, 0, ops[0].reg, byte_needs_rex(&ops[0]));
        e8(s, 0x0F);
        e8(s, (uint8_t)(0x90 + cc));
        modrm_rr(s, 0, ops[0].reg);
        return;
    }

    /* SSE table */
    for (i = 0; sse_table[i].name; i++) {
        if (strcmp(mnem, sse_table[i].name))
            continue;
        {
            const struct sse_form *f = &sse_table[i];
            struct x64_operand *reg = &ops[0], *rm = &ops[1];
            uint8_t op = f->op;
            /* movsd/movss store: [mem], xmm uses the 0x11 store opcode with the
               xmm in the reg field. */
            if ((!strcmp(mnem, "movsd") || !strcmp(mnem, "movss")) &&
                ops[0].kind == XO_MEM) {
                enc_sse(a, s, f->pfx, 0x11, 0, &ops[1], &ops[0], emit);
                return;
            }
            /* movq / movd handled separately below */
            enc_sse(a, s, f->pfx, op, f->w, reg, rm, emit);
            return;
        }
    }

    if (!strcmp(mnem, "movq") || !strcmp(mnem, "movd")) {
        int w = !strcmp(mnem, "movq");
        /* dst xmm <- gpr : 66 [REX.W] 0F 6E ; gpr <- xmm : 66 [REX.W] 0F 7E */
        struct x64_operand *d = &ops[0], *srco = &ops[1];
        e8(s, 0x66);
        if (d->rclass == RC_XMM) {               /* xmm <- gpr (6E) */
            rex(s, w, d->reg, 0, srco->reg, 0);
            e8(s, 0x0F); e8(s, 0x6E);
            modrm_rr(s, d->reg, srco->reg);
        } else {                                 /* gpr <- xmm (7E) */
            rex(s, w, srco->reg, 0, d->reg, 0);
            e8(s, 0x0F); e8(s, 0x7E);
            modrm_rr(s, srco->reg, d->reg);
        }
        return;
    }

    die("x86-64: unknown mnemonic '%s'", mnem);
}
