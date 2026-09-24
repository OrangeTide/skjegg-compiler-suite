/* emit_x86.h : x86-64 machine-code encoder (byte emitter).

   The byte-level counterpart of backend/x86_emit.c, which emits GAS text for
   the AOT path.  This encoder writes raw machine code into a growable buffer,
   the form an in-process JIT needs.  Every encoder here is checked byte for
   byte against nasm by tests/x86_oracle.c. */

#ifndef EMIT_X86_H
#define EMIT_X86_H

#include <stddef.h>
#include <stdint.h>

/* Hardware register numbers, as they appear in ModRM/REX encodings. */
enum x_reg {
    X_RAX = 0, X_RCX = 1, X_RDX = 2, X_RBX = 3,
    X_RSP = 4, X_RBP = 5, X_RSI = 6, X_RDI = 7,
    X_R8 = 8, X_R9 = 9, X_R10 = 10, X_R11 = 11,
    X_R12 = 12, X_R13 = 13, X_R14 = 14, X_R15 = 15,
};

/* SSE register numbers; encoded the same way GPRs are in ModRM/REX. */
enum x_xmm {
    X_XMM0 = 0, X_XMM1, X_XMM2, X_XMM3, X_XMM4, X_XMM5, X_XMM6, X_XMM7,
    X_XMM8, X_XMM9, X_XMM10, X_XMM11, X_XMM12, X_XMM13, X_XMM14, X_XMM15,
};

/* Condition codes, the low nibble of the Jcc/SETcc opcodes. */
enum x_cc {
    X_O = 0x0, X_NO = 0x1, X_B = 0x2, X_AE = 0x3,
    X_E = 0x4, X_NE = 0x5, X_BE = 0x6, X_A = 0x7,
    X_S = 0x8, X_NS = 0x9, X_P = 0xA, X_NP = 0xB,
    X_L = 0xC, X_GE = 0xD, X_LE = 0xE, X_G = 0xF,
};

/* A growable byte buffer that the encoders append into. Backed by malloc;
   the JIT copies the finished bytes into an executable mapping. */
struct code {
    uint8_t *buf;
    size_t len;
    size_t cap;
};

void code_init(struct code *c);
void code_free(struct code *c);
void emit8(struct code *c, uint8_t b);
void emit32(struct code *c, uint32_t v);
void emit64(struct code *c, uint64_t v);

/* Data movement. The _ri32 form is a 32-bit load that zero-extends to 64
   bits (the natural encoding for a Kobold i32 constant); _ri64 loads a full
   64-bit immediate (a global address or an import pointer). */
void x_mov_ri32(struct code *c, int reg, uint32_t imm);
void x_mov_ri64(struct code *c, int reg, uint64_t imm);
void x_mov_rr(struct code *c, int dst, int src);

/* 64-bit integer arithmetic and logic, dst = dst OP src. */
void x_add_rr(struct code *c, int dst, int src);
void x_sub_rr(struct code *c, int dst, int src);
void x_and_rr(struct code *c, int dst, int src);
void x_or_rr(struct code *c, int dst, int src);
void x_xor_rr(struct code *c, int dst, int src);
void x_imul_rr(struct code *c, int dst, int src);
void x_neg_r(struct code *c, int reg);
void x_not_r(struct code *c, int reg);

/* 32-bit forms of the same, for Kobold i32 values: the operation runs on the
   low 32 bits and zero-extends the result, so a signed 32-bit compare stays
   correct regardless of the upper bits. */
void x_mov32_rr(struct code *c, int dst, int src);
void x_add32_rr(struct code *c, int dst, int src);
void x_sub32_rr(struct code *c, int dst, int src);
void x_and32_rr(struct code *c, int dst, int src);
void x_or32_rr(struct code *c, int dst, int src);
void x_xor32_rr(struct code *c, int dst, int src);
void x_imul32_rr(struct code *c, int dst, int src);
void x_neg32_r(struct code *c, int reg);
void x_not32_r(struct code *c, int reg);
void x_cmp32_rr(struct code *c, int a, int b);
void x_test32_rr(struct code *c, int a, int b);

/* dst OP= imm32 (sign-extended). */
void x_add_ri32(struct code *c, int reg, int32_t imm);
void x_sub_ri32(struct code *c, int reg, int32_t imm);

/* Signed division: cqo sign-extends rax into rdx, idiv divides rdx:rax by
   reg leaving the quotient in rax and the remainder in rdx. */
void x_cqo(struct code *c);
void x_idiv_r(struct code *c, int reg);
/* cdq is the 32-bit sign-extend (eax into edx:eax) that precedes idiv32. */
void x_cdq(struct code *c);
void x_idiv32_r(struct code *c, int reg);
/* Unsigned divide: edx:eax / r (32-bit) or rdx:rax / r (64-bit); zero rdx/edx
   first (not cdq/cqo).  Quotient in rax, remainder in rdx. */
void x_div_r(struct code *c, int reg);
void x_div32_r(struct code *c, int reg);

/* Shifts by the count in cl. */
void x_shl_cl(struct code *c, int reg);
void x_sar_cl(struct code *c, int reg);
void x_shr_cl(struct code *c, int reg);
void x_shl32_cl(struct code *c, int reg);
void x_sar32_cl(struct code *c, int reg);
void x_shr32_cl(struct code *c, int reg);

/* Compare and condition materialization. cmp sets flags; setcc writes a 0/1
   byte into the low byte of reg; movzx clears the upper bits. */
void x_cmp_rr(struct code *c, int a, int b);
void x_test_rr(struct code *c, int a, int b);
void x_setcc_r(struct code *c, int cc, int reg);
void x_movzx_rb(struct code *c, int dst, int src);
void x_movsxd_rr(struct code *c, int dst, int src);

/* Load one byte, zero-extended into a 32-bit result (a string character read). */
void x_load8(struct code *c, int dst, int base, int32_t disp);
/* Sub-word loads into a 32-bit result: byte/half, sign- or zero-extended. */
void x_load8s(struct code *c, int dst, int base, int32_t disp);
void x_load16(struct code *c, int dst, int base, int32_t disp);
void x_load16s(struct code *c, int dst, int base, int32_t disp);
/* Sub-word stores of a register's low byte / low 16 bits. */
void x_store8(struct code *c, int base, int32_t disp, int src);
void x_store16(struct code *c, int base, int32_t disp, int src);
/* 32-bit memory access (i32 loads and stores). */
void x_load32(struct code *c, int dst, int base, int32_t disp);
void x_store32(struct code *c, int base, int32_t disp, int src);
/* 64-bit memory access (slots wide enough to hold a pointer or i64). */
void x_load64(struct code *c, int dst, int base, int32_t disp);
void x_store64(struct code *c, int base, int32_t disp, int src);
/* dst = base + disp, the address itself (for the address of a local slot). */
void x_lea(struct code *c, int dst, int base, int32_t disp);

/* Stack. */
void x_push_r(struct code *c, int reg);
void x_pop_r(struct code *c, int reg);

/* Control flow. The rel32-relative encoders leave a zero displacement and
   return the buffer offset of that displacement field for later backpatch;
   the target is patched with x_patch_rel32 once its address is known. */
void x_ret(struct code *c);
void x_leave(struct code *c);
void x_nop(struct code *c);
void x_call_r(struct code *c, int reg);
void x_jmp_r(struct code *c, int reg);
size_t x_call_rel32(struct code *c);
size_t x_jmp_rel32(struct code *c);
size_t x_jcc_rel32(struct code *c, int cc);

/* Patch a rel32 field (at buffer offset `at`, as returned above) so the
   instruction jumps to buffer offset `target`. */
void x_patch_rel32(struct code *c, size_t at, size_t target);

/* Double-precision SSE: register moves, 64-bit loads and stores, the four
   arithmetic ops (dst OP= src), an ordered compare that sets the integer
   flags, and signed int32-to-double conversion. */
void x_movsd_rr(struct code *c, int dst, int src);
void x_movsd_load(struct code *c, int dst, int base, int32_t disp);
void x_movsd_store(struct code *c, int base, int32_t disp, int src);
void x_addsd_rr(struct code *c, int dst, int src);
void x_subsd_rr(struct code *c, int dst, int src);
void x_mulsd_rr(struct code *c, int dst, int src);
void x_divsd_rr(struct code *c, int dst, int src);
void x_ucomisd_rr(struct code *c, int a, int b);
void x_cvtsi2sd(struct code *c, int xmm_dst, int gpr_src);
void x_movss_store(struct code *c, int base, int32_t disp, int src);
void x_movss_load(struct code *c, int dst, int base, int32_t disp);
void x_cvtss2sd_rr(struct code *c, int xmm_dst, int xmm_src);
void x_cvtsd2ss_rr(struct code *c, int xmm_dst, int xmm_src);
void x_cvtss2sd_load(struct code *c, int xmm_dst, int base, int32_t disp);

/* Single-precision (f32-in-register) arithmetic, ordered compare, and the two
   int32<->f32 converts, the F3-prefixed twins of the scalar-double ops. */
void x_addss_rr(struct code *c, int dst, int src);
void x_subss_rr(struct code *c, int dst, int src);
void x_mulss_rr(struct code *c, int dst, int src);
void x_divss_rr(struct code *c, int dst, int src);
void x_ucomiss_rr(struct code *c, int a, int b);
void x_cvtsi2ss(struct code *c, int xmm_dst, int gpr_src);
void x_cvttss2si(struct code *c, int gpr_dst, int xmm_src);

/* sqrtsd (scalar-double square root) and the two f64->i32 converts:
   cvttsd2si truncates toward zero, cvtsd2si rounds to nearest. The converts
   write a 32-bit GPR from an xmm source. */
void x_sqrtsd_rr(struct code *c, int dst, int src);
void x_cvttsd2si(struct code *c, int gpr_dst, int xmm_src);
void x_cvtsd2si(struct code *c, int gpr_dst, int xmm_src);

/* movq between a 64-bit GPR and an xmm register (66 REX.W 0F 7E / 0F 6E). */
void x_movq_from_xmm(struct code *c, int gpr_dst, int xmm_src);
void x_movq_to_xmm(struct code *c, int xmm_dst, int gpr_src);

/* movd: the low 32 bits between a 32-bit GPR and an xmm (66 0F 7E / 0F 6E). */
void x_movd_from_xmm(struct code *c, int gpr_dst, int xmm_src);
void x_movd_to_xmm(struct code *c, int xmm_dst, int gpr_src);

#endif /* EMIT_X86_H */
