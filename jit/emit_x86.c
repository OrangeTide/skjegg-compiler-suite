/* emit_x86.c : x86-64 machine-code encoder (byte emitter).

   The byte-level counterpart of backend/x86_emit.c (which emits GAS text for
   the AOT path).  Checked byte for byte against nasm by tests/x86_oracle.c. */

#include "emit_x86.h"

#include <stdlib.h>
#include <string.h>

/****************************************************************
 * Byte buffer
 ****************************************************************/

void
code_init(struct code *c)
{
    c->cap = 256;
    c->buf = malloc(c->cap);
    c->len = 0;
}

void
code_free(struct code *c)
{
    free(c->buf);
    c->buf = NULL;
    c->len = c->cap = 0;
}

static void
grow(struct code *c, size_t need)
{
    if (c->len + need <= c->cap)
        return;
    while (c->len + need > c->cap)
        c->cap *= 2;
    c->buf = realloc(c->buf, c->cap);
}

void
emit8(struct code *c, uint8_t b)
{
    grow(c, 1);
    c->buf[c->len++] = b;
}

void
emit32(struct code *c, uint32_t v)
{
    grow(c, 4);
    c->buf[c->len++] = (uint8_t)v;
    c->buf[c->len++] = (uint8_t)(v >> 8);
    c->buf[c->len++] = (uint8_t)(v >> 16);
    c->buf[c->len++] = (uint8_t)(v >> 24);
}

void
emit64(struct code *c, uint64_t v)
{
    emit32(c, (uint32_t)v);
    emit32(c, (uint32_t)(v >> 32));
}

/****************************************************************
 * ModRM / REX helpers
 ****************************************************************/

/* Emit a REX prefix when the operation is 64-bit (w), any operand names an
   extended register (reg/index/base bit 3), or a byte-register operand
   forces one (spl/bpl/sil/dil have no legacy encoding). */
static void
rex(struct code *c, int w, int reg, int idx, int rm, int force)
{
    int r = (reg >> 3) & 1;
    int x = (idx >> 3) & 1;
    int b = (rm >> 3) & 1;

    if (w || r || x || b || force)
        emit8(c, (uint8_t)(0x40 | (w << 3) | (r << 2) | (x << 1) | b));
}

/* Register-direct ModRM: both operands are registers (mod = 11). */
static void
modrm_rr(struct code *c, int reg, int rm)
{
    emit8(c, (uint8_t)(0xC0 | ((reg & 7) << 3) | (rm & 7)));
}

/* Memory ModRM for [base + disp], with `reg` in the reg field. Handles the
   rsp/r12 SIB requirement and the rbp/r13 no-zero-disp quirk. */
static void
modrm_mem(struct code *c, int reg, int base, int32_t disp)
{
    int bb = base & 7;
    int mod;

    if (disp == 0 && bb != 5)   /* bb==5 is rbp/r13: needs a disp */
        mod = 0;
    else if (disp >= -128 && disp <= 127)
        mod = 1;
    else
        mod = 2;

    if (bb == 4) {              /* rsp/r12: escape to SIB */
        emit8(c, (uint8_t)((mod << 6) | ((reg & 7) << 3) | 4));
        emit8(c, 0x24);        /* scale 0, index none, base rsp/r12 */
    } else {
        emit8(c, (uint8_t)((mod << 6) | ((reg & 7) << 3) | bb));
    }
    if (mod == 1)
        emit8(c, (uint8_t)disp);
    else if (mod == 2)
        emit32(c, (uint32_t)disp);
}

/****************************************************************
 * Data movement
 ****************************************************************/

void
x_mov_ri32(struct code *c, int reg, uint32_t imm)
{
    rex(c, 0, 0, 0, reg, 0);
    emit8(c, (uint8_t)(0xB8 + (reg & 7)));
    emit32(c, imm);
}

void
x_mov_ri64(struct code *c, int reg, uint64_t imm)
{
    rex(c, 1, 0, 0, reg, 0);
    emit8(c, (uint8_t)(0xB8 + (reg & 7)));
    emit64(c, imm);
}

/* dst = src, and the r/r arithmetic below, all share the "OP r/m, r" shape:
   reg field = src, r/m field = dst. `w` selects the 64-bit operand size; a
   32-bit op (w=0) zero-extends its result into the full register. */
static void
alu_rr_w(struct code *c, uint8_t op, int dst, int src, int w)
{
    rex(c, w, src, 0, dst, 0);
    emit8(c, op);
    modrm_rr(c, src, dst);
}

void x_mov_rr(struct code *c, int dst, int src) { alu_rr_w(c, 0x89, dst, src, 1); }
void x_add_rr(struct code *c, int dst, int src) { alu_rr_w(c, 0x01, dst, src, 1); }
void x_sub_rr(struct code *c, int dst, int src) { alu_rr_w(c, 0x29, dst, src, 1); }
void x_and_rr(struct code *c, int dst, int src) { alu_rr_w(c, 0x21, dst, src, 1); }
void x_or_rr(struct code *c, int dst, int src)  { alu_rr_w(c, 0x09, dst, src, 1); }
void x_xor_rr(struct code *c, int dst, int src) { alu_rr_w(c, 0x31, dst, src, 1); }

void x_mov32_rr(struct code *c, int dst, int src) { alu_rr_w(c, 0x89, dst, src, 0); }
void x_add32_rr(struct code *c, int dst, int src) { alu_rr_w(c, 0x01, dst, src, 0); }
void x_sub32_rr(struct code *c, int dst, int src) { alu_rr_w(c, 0x29, dst, src, 0); }
void x_and32_rr(struct code *c, int dst, int src) { alu_rr_w(c, 0x21, dst, src, 0); }
void x_or32_rr(struct code *c, int dst, int src)  { alu_rr_w(c, 0x09, dst, src, 0); }
void x_xor32_rr(struct code *c, int dst, int src) { alu_rr_w(c, 0x31, dst, src, 0); }
void x_cmp32_rr(struct code *c, int a, int b)     { alu_rr_w(c, 0x39, a, b, 0); }
void x_test32_rr(struct code *c, int a, int b)    { alu_rr_w(c, 0x85, a, b, 0); }

static void
imul_rr_w(struct code *c, int dst, int src, int w)
{
    /* imul r, r/m: reg field = dst, r/m = src (0F AF /r) */
    rex(c, w, dst, 0, src, 0);
    emit8(c, 0x0F);
    emit8(c, 0xAF);
    modrm_rr(c, dst, src);
}

void x_imul_rr(struct code *c, int dst, int src)   { imul_rr_w(c, dst, src, 1); }
void x_imul32_rr(struct code *c, int dst, int src) { imul_rr_w(c, dst, src, 0); }

static void
unary_w(struct code *c, int ext, int reg, int w)
{
    rex(c, w, 0, 0, reg, 0);
    emit8(c, 0xF7);
    modrm_rr(c, ext, reg);
}

void x_neg_r(struct code *c, int reg)   { unary_w(c, 3, reg, 1); }  /* /3 NEG */
void x_not_r(struct code *c, int reg)   { unary_w(c, 2, reg, 1); }  /* /2 NOT */
void x_neg32_r(struct code *c, int reg) { unary_w(c, 3, reg, 0); }
void x_not32_r(struct code *c, int reg) { unary_w(c, 2, reg, 0); }

static void
alu_ri32(struct code *c, int ext, int reg, int32_t imm)
{
    rex(c, 1, 0, 0, reg, 0);
    emit8(c, 0x81);
    modrm_rr(c, ext, reg);
    emit32(c, (uint32_t)imm);
}

void x_add_ri32(struct code *c, int reg, int32_t imm) { alu_ri32(c, 0, reg, imm); }
void x_sub_ri32(struct code *c, int reg, int32_t imm) { alu_ri32(c, 5, reg, imm); }

void
x_cqo(struct code *c)
{
    emit8(c, 0x48);            /* REX.W */
    emit8(c, 0x99);
}

void
x_cdq(struct code *c)
{
    emit8(c, 0x99);            /* sign-extend eax into edx:eax */
}

static void
divmod_w(struct code *c, int reg, int w, int ext)
{
    rex(c, w, 0, 0, reg, 0);
    emit8(c, 0xF7);
    modrm_rr(c, ext, reg);     /* /7 = IDIV (signed), /6 = DIV (unsigned) */
}

void x_idiv_r(struct code *c, int reg)   { divmod_w(c, reg, 1, 7); }
void x_idiv32_r(struct code *c, int reg) { divmod_w(c, reg, 0, 7); }
void x_div_r(struct code *c, int reg)    { divmod_w(c, reg, 1, 6); }
void x_div32_r(struct code *c, int reg)  { divmod_w(c, reg, 0, 6); }

static void
shift_cl_w(struct code *c, int ext, int reg, int w)
{
    rex(c, w, 0, 0, reg, 0);
    emit8(c, 0xD3);
    modrm_rr(c, ext, reg);
}

void x_shl_cl(struct code *c, int reg) { shift_cl_w(c, 4, reg, 1); }
void x_shr_cl(struct code *c, int reg) { shift_cl_w(c, 5, reg, 1); }
void x_sar_cl(struct code *c, int reg) { shift_cl_w(c, 7, reg, 1); }
void x_shl32_cl(struct code *c, int reg) { shift_cl_w(c, 4, reg, 0); }
void x_shr32_cl(struct code *c, int reg) { shift_cl_w(c, 5, reg, 0); }
void x_sar32_cl(struct code *c, int reg) { shift_cl_w(c, 7, reg, 0); }

void
x_cmp_rr(struct code *c, int a, int b)
{
    /* cmp r/m64, r64: reg field = b, r/m = a (computes a - b for flags) */
    alu_rr_w(c, 0x39, a, b, 1);
}

void
x_test_rr(struct code *c, int a, int b)
{
    alu_rr_w(c, 0x85, a, b, 1);
}

/* A byte-register operand needs a forced REX only for spl/bpl/sil/dil
   (regs 4-7); al..bl take no REX and r8b..r15b get REX from their high bit. */
static int
byte_needs_rex(int reg)
{
    return reg >= X_RSP && reg <= X_RDI;
}

void
x_setcc_r(struct code *c, int cc, int reg)
{
    rex(c, 0, 0, 0, reg, byte_needs_rex(reg));
    emit8(c, 0x0F);
    emit8(c, (uint8_t)(0x90 + cc));
    modrm_rr(c, 0, reg);
}

void
x_movzx_rb(struct code *c, int dst, int src)
{
    /* movzx r64, r/m8: reg = dst, r/m = src (0F B6 /r) */
    rex(c, 1, dst, 0, src, byte_needs_rex(src));
    emit8(c, 0x0F);
    emit8(c, 0xB6);
    modrm_rr(c, dst, src);
}

void
x_movsxd_rr(struct code *c, int dst, int src)
{
    /* movsxd r64, r/m32: sign-extend the low 32 bits of src into the 64-bit dst
       (REX.W 63 /r), reg = dst, r/m = src */
    rex(c, 1, dst, 0, src, 0);
    emit8(c, 0x63);
    modrm_rr(c, dst, src);
}

/****************************************************************
 * Memory access
 ****************************************************************/

void
x_load8(struct code *c, int dst, int base, int32_t disp)
{
    /* movzx r32, byte ptr [base+disp]: read one byte, zero-extended. The r/m is memory, so
       the byte-register REX rule (for SIL/DIL) does not apply. */
    rex(c, 0, dst, 0, base, 0);
    emit8(c, 0x0F);
    emit8(c, 0xB6);           /* 0F B6 /r : movzx r32, r/m8 */
    modrm_mem(c, dst, base, disp);
}

void
x_load8s(struct code *c, int dst, int base, int32_t disp)
{
    /* movsx r32, byte ptr [base+disp]: read one byte, sign-extended. */
    rex(c, 0, dst, 0, base, 0);
    emit8(c, 0x0F);
    emit8(c, 0xBE);           /* 0F BE /r : movsx r32, r/m8 */
    modrm_mem(c, dst, base, disp);
}

void
x_load16(struct code *c, int dst, int base, int32_t disp)
{
    /* movzx r32, word ptr [base+disp]: read two bytes, zero-extended. */
    rex(c, 0, dst, 0, base, 0);
    emit8(c, 0x0F);
    emit8(c, 0xB7);           /* 0F B7 /r : movzx r32, r/m16 */
    modrm_mem(c, dst, base, disp);
}

void
x_load16s(struct code *c, int dst, int base, int32_t disp)
{
    /* movsx r32, word ptr [base+disp]: read two bytes, sign-extended. */
    rex(c, 0, dst, 0, base, 0);
    emit8(c, 0x0F);
    emit8(c, 0xBF);           /* 0F BF /r : movsx r32, r/m16 */
    modrm_mem(c, dst, base, disp);
}

void
x_load32(struct code *c, int dst, int base, int32_t disp)
{
    rex(c, 0, dst, 0, base, 0);
    emit8(c, 0x8B);            /* mov r32, r/m32 */
    modrm_mem(c, dst, base, disp);
}

void
x_store8(struct code *c, int base, int32_t disp, int src)
{
    /* mov byte ptr [base+disp], src8.  force REX when src is SPL/BPL/SIL/DIL
       so the low byte is addressed, not AH/CH/DH/BH. */
    rex(c, 0, src, 0, base, byte_needs_rex(src));
    emit8(c, 0x88);           /* 88 /r : mov r/m8, r8 */
    modrm_mem(c, src, base, disp);
}

void
x_store16(struct code *c, int base, int32_t disp, int src)
{
    /* mov word ptr [base+disp], src16: the 0x66 operand-size prefix precedes
       any REX byte. */
    emit8(c, 0x66);
    rex(c, 0, src, 0, base, 0);
    emit8(c, 0x89);           /* 89 /r : mov r/m16, r16 (with 66 prefix) */
    modrm_mem(c, src, base, disp);
}

void
x_store32(struct code *c, int base, int32_t disp, int src)
{
    rex(c, 0, src, 0, base, 0);
    emit8(c, 0x89);            /* mov r/m32, r32 */
    modrm_mem(c, src, base, disp);
}

void
x_load64(struct code *c, int dst, int base, int32_t disp)
{
    rex(c, 1, dst, 0, base, 0);
    emit8(c, 0x8B);
    modrm_mem(c, dst, base, disp);
}

void
x_store64(struct code *c, int base, int32_t disp, int src)
{
    rex(c, 1, src, 0, base, 0);
    emit8(c, 0x89);
    modrm_mem(c, src, base, disp);
}

void
x_lea(struct code *c, int dst, int base, int32_t disp)
{
    rex(c, 1, dst, 0, base, 0);
    emit8(c, 0x8D);            /* lea r64, m */
    modrm_mem(c, dst, base, disp);
}

/****************************************************************
 * Stack and control flow
 ****************************************************************/

void
x_push_r(struct code *c, int reg)
{
    rex(c, 0, 0, 0, reg, 0);
    emit8(c, (uint8_t)(0x50 + (reg & 7)));
}

void
x_pop_r(struct code *c, int reg)
{
    rex(c, 0, 0, 0, reg, 0);
    emit8(c, (uint8_t)(0x58 + (reg & 7)));
}

void x_ret(struct code *c)   { emit8(c, 0xC3); }
void x_leave(struct code *c) { emit8(c, 0xC9); }
void x_nop(struct code *c)   { emit8(c, 0x90); }

void
x_call_r(struct code *c, int reg)
{
    rex(c, 0, 0, 0, reg, 0);
    emit8(c, 0xFF);
    modrm_rr(c, 2, reg);       /* /2 = CALL r/m64 */
}

void
x_jmp_r(struct code *c, int reg)
{
    rex(c, 0, 0, 0, reg, 0);
    emit8(c, 0xFF);
    modrm_rr(c, 4, reg);       /* /4 = JMP r/m64 */
}

size_t
x_call_rel32(struct code *c)
{
    size_t at;
    emit8(c, 0xE8);
    at = c->len;
    emit32(c, 0);
    return at;
}

size_t
x_jmp_rel32(struct code *c)
{
    size_t at;
    emit8(c, 0xE9);
    at = c->len;
    emit32(c, 0);
    return at;
}

size_t
x_jcc_rel32(struct code *c, int cc)
{
    size_t at;
    emit8(c, 0x0F);
    emit8(c, (uint8_t)(0x80 + cc));
    at = c->len;
    emit32(c, 0);
    return at;
}

void
x_patch_rel32(struct code *c, size_t at, size_t target)
{
    /* rel32 is measured from the end of the instruction, which is the end
       of this 4-byte displacement field. */
    int32_t rel = (int32_t)(target - (at + 4));
    c->buf[at + 0] = (uint8_t)rel;
    c->buf[at + 1] = (uint8_t)(rel >> 8);
    c->buf[at + 2] = (uint8_t)(rel >> 16);
    c->buf[at + 3] = (uint8_t)(rel >> 24);
}

/****************************************************************
 * Double-precision SSE
 *
 * The mandatory prefix (F2 for scalar-double, 66 for the compare) comes
 * first, then any REX for an extended register, then 0F and the opcode.
 ****************************************************************/

static void
sse_rr(struct code *c, uint8_t pfx, uint8_t op, int reg, int rm)
{
    if (pfx)                     /* 0 means no mandatory prefix (ucomiss) */
        emit8(c, pfx);
    rex(c, 0, reg, 0, rm, 0);
    emit8(c, 0x0F);
    emit8(c, op);
    modrm_rr(c, reg, rm);
}

static void
sse_mem(struct code *c, uint8_t pfx, uint8_t op, int reg, int base, int32_t disp)
{
    emit8(c, pfx);
    rex(c, 0, reg, 0, base, 0);
    emit8(c, 0x0F);
    emit8(c, op);
    modrm_mem(c, reg, base, disp);
}

void x_movsd_rr(struct code *c, int dst, int src) { sse_rr(c, 0xF2, 0x10, dst, src); }
void x_addsd_rr(struct code *c, int dst, int src) { sse_rr(c, 0xF2, 0x58, dst, src); }
void x_subsd_rr(struct code *c, int dst, int src) { sse_rr(c, 0xF2, 0x5C, dst, src); }
void x_mulsd_rr(struct code *c, int dst, int src) { sse_rr(c, 0xF2, 0x59, dst, src); }
void x_divsd_rr(struct code *c, int dst, int src) { sse_rr(c, 0xF2, 0x5E, dst, src); }
void x_ucomisd_rr(struct code *c, int a, int b)   { sse_rr(c, 0x66, 0x2E, a, b); }

/* cvtsi2sd from a 32-bit source (no REX.W): reg = xmm dst, r/m = gpr src. */
void x_cvtsi2sd(struct code *c, int xmm_dst, int gpr_src)
{
    sse_rr(c, 0xF2, 0x2A, xmm_dst, gpr_src);
}

void
x_movsd_load(struct code *c, int dst, int base, int32_t disp)
{
    sse_mem(c, 0xF2, 0x10, dst, base, disp);
}

void
x_movsd_store(struct code *c, int base, int32_t disp, int src)
{
    sse_mem(c, 0xF2, 0x11, src, base, disp);
}

/* Single precision. movss moves a 4-byte f32; cvtss2sd widens f32 to f64 and
 * cvtsd2ss narrows f64 to f32, both reg-reg. x_cvtss2sd_load widens a 4-byte
 * f32 read straight from memory, which is the single-load fast path. */
void x_movss_store(struct code *c, int base, int32_t disp, int src)
{
    sse_mem(c, 0xF3, 0x11, src, base, disp);
}

void x_movss_load(struct code *c, int dst, int base, int32_t disp)
{
    sse_mem(c, 0xF3, 0x10, dst, base, disp);
}

void x_cvtss2sd_rr(struct code *c, int xmm_dst, int xmm_src)
{
    sse_rr(c, 0xF3, 0x5A, xmm_dst, xmm_src);
}

void x_cvtsd2ss_rr(struct code *c, int xmm_dst, int xmm_src)
{
    sse_rr(c, 0xF2, 0x5A, xmm_dst, xmm_src);
}

void x_cvtss2sd_load(struct code *c, int xmm_dst, int base, int32_t disp)
{
    sse_mem(c, 0xF3, 0x5A, xmm_dst, base, disp);
}

/* Single-precision arithmetic and conversions: the F3-prefixed twins of the
 * scalar-double ops, for f32 values the C front end keeps as f32 in a register.
 * ucomiss carries no mandatory prefix (the sd form is 66-prefixed). */
void x_addss_rr(struct code *c, int dst, int src) { sse_rr(c, 0xF3, 0x58, dst, src); }
void x_subss_rr(struct code *c, int dst, int src) { sse_rr(c, 0xF3, 0x5C, dst, src); }
void x_mulss_rr(struct code *c, int dst, int src) { sse_rr(c, 0xF3, 0x59, dst, src); }
void x_divss_rr(struct code *c, int dst, int src) { sse_rr(c, 0xF3, 0x5E, dst, src); }
void x_ucomiss_rr(struct code *c, int a, int b)   { sse_rr(c, 0x00, 0x2E, a, b); }
void x_cvtsi2ss(struct code *c, int xmm_dst, int gpr_src) { sse_rr(c, 0xF3, 0x2A, xmm_dst, gpr_src); }
void x_cvttss2si(struct code *c, int gpr_dst, int xmm_src) { sse_rr(c, 0xF3, 0x2C, gpr_dst, xmm_src); }

/* sqrtsd and the two rounding converts share the F2 scalar-double form. For the
   converts the reg field is the GPR and the r/m field is the xmm source; a
   32-bit destination takes no REX.W, matching the 32-bit integer result. */
void x_sqrtsd_rr(struct code *c, int dst, int src) { sse_rr(c, 0xF2, 0x51, dst, src); }
void x_cvttsd2si(struct code *c, int gpr_dst, int xmm_src) { sse_rr(c, 0xF2, 0x2C, gpr_dst, xmm_src); }
void x_cvtsd2si(struct code *c, int gpr_dst, int xmm_src)  { sse_rr(c, 0xF2, 0x2D, gpr_dst, xmm_src); }

/* movq r64 <- xmm (0F 7E) and xmm <- r64 (0F 6E). Both are 66-prefixed and take
   REX.W; the xmm register sits in the ModRM reg field, the GPR in r/m. */
void
x_movq_from_xmm(struct code *c, int gpr_dst, int xmm_src)
{
    emit8(c, 0x66);
    rex(c, 1, xmm_src, 0, gpr_dst, 0);
    emit8(c, 0x0F);
    emit8(c, 0x7E);
    modrm_rr(c, xmm_src, gpr_dst);
}

void
x_movq_to_xmm(struct code *c, int xmm_dst, int gpr_src)
{
    emit8(c, 0x66);
    rex(c, 1, xmm_dst, 0, gpr_src, 0);
    emit8(c, 0x0F);
    emit8(c, 0x6E);
    modrm_rr(c, xmm_dst, gpr_src);
}

/* movd is movq without REX.W: it moves the low 32 bits between an xmm and a
   32-bit GPR (the _Float16 helper glue moves single-precision bits). */
void
x_movd_from_xmm(struct code *c, int gpr_dst, int xmm_src)
{
    emit8(c, 0x66);
    rex(c, 0, xmm_src, 0, gpr_dst, 0);
    emit8(c, 0x0F);
    emit8(c, 0x7E);
    modrm_rr(c, xmm_src, gpr_dst);
}

void
x_movd_to_xmm(struct code *c, int xmm_dst, int gpr_src)
{
    emit8(c, 0x66);
    rex(c, 0, xmm_dst, 0, gpr_src, 0);
    emit8(c, 0x0F);
    emit8(c, 0x6E);
    modrm_rr(c, xmm_dst, gpr_src);
}
