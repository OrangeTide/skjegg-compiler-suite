/* arm64_mc.h : the AArch64 machine-code sink, the output abstraction shared by
   the AArch64 instruction selector (backend/arm64_select.c).

   One selector drives two sinks.  The text sink (backend/arm64_mc_text.c) writes
   GAS assembly, the form the AOT compiler emits.  The byte sink (jit/jit_arm64.c)
   writes machine code into a growable buffer, the form the in-process JIT runs.
   Both implement the operations declared here, so the selector is written once
   against hardware register numbers and calls mc64_* without knowing which sink
   it drives.  This is the AArch64 parallel of backend/mc.h (x86-64).

   Registers are hardware numbers 0..31.  For most instructions 31 is the zero
   register; for add/sub-immediate and load/store base it is the stack pointer.
   The width of an integer operation rides its sf flag (0 = 32-bit w-view, which
   zeroes the upper half and keeps an i32 address-clean; 1 = 64-bit x-view), not
   the register.  Floating-point width rides sz (2 = single, 3 = double, 1 =
   half).  Control flow is by label id; a call is by symbol (a local module
   function or a runtime/host binding) or through a register.  The three "smart"
   operations mc64_movimm, mc64_lea_sym, and the load/store family hide the forms
   that differ per sink: the text sink leans on the assembler (ldr =imm, ldr =sym,
   scaled offsets); the byte sink materialises them (movz/movk, offset range
   selection, a scratch-register fallback). */

#ifndef ARM64_MC_H
#define ARM64_MC_H

#include <stddef.h>
#include <stdint.h>

/* Named registers the selector and glue use. */
enum {
    MC64_FP = 29,   /* frame pointer (x29) */
    MC64_LR = 30,   /* link register (x30) */
    MC64_SP = 31,   /* stack pointer (add/sub-imm and ldst base) */
    MC64_ZR = 31,   /* zero register (elsewhere) */
};

/* Condition codes (the AArch64 4-bit field). */
enum mc64_cc {
    MC64_EQ = 0x0, MC64_NE = 0x1, MC64_CS = 0x2, MC64_CC = 0x3,
    MC64_MI = 0x4, MC64_PL = 0x5, MC64_VS = 0x6, MC64_VC = 0x7,
    MC64_HI = 0x8, MC64_LS = 0x9, MC64_GE = 0xA, MC64_LT = 0xB,
    MC64_GT = 0xC, MC64_LE = 0xD, MC64_AL = 0xE,
};

/* The sink is opaque here; each implementation defines its own struct mc64. */
struct mc64;

/* ---- Control flow by label id (forward references allowed). */
int mc64_new_label(struct mc64 *m);
void mc64_label(struct mc64 *m, int id);
void mc64_b(struct mc64 *m, int id);
void mc64_cbz(struct mc64 *m, int sf, int Rt, int id);
void mc64_cbnz(struct mc64 *m, int sf, int Rt, int id);
void mc64_ret(struct mc64 *m);
void mc64_blr(struct mc64 *m, int Rn);
void mc64_br(struct mc64 *m, int Rn);

/* ---- Moves and immediates.  mc64_mov is a register copy; mc64_movimm loads an
   arbitrary constant; mc64_lea_sym loads a data global's or function's address. */
void mc64_mov(struct mc64 *m, int sf, int Rd, int Rn);
void mc64_movimm(struct mc64 *m, int sf, int Rd, uint64_t v);
void mc64_lea_sym(struct mc64 *m, int sf, int Rd, const char *name);

/* ---- Address arithmetic: Rd = Rn + imm for any signed imm (64-bit view). */
void mc64_addimm(struct mc64 *m, int Rd, int Rn, long imm);

/* ---- Integer arithmetic and logic, Rd = Rn OP Rm. */
void mc64_add(struct mc64 *m, int sf, int Rd, int Rn, int Rm);
void mc64_sub(struct mc64 *m, int sf, int Rd, int Rn, int Rm);
void mc64_and(struct mc64 *m, int sf, int Rd, int Rn, int Rm);
void mc64_orr(struct mc64 *m, int sf, int Rd, int Rn, int Rm);
void mc64_eor(struct mc64 *m, int sf, int Rd, int Rn, int Rm);
void mc64_mul(struct mc64 *m, int sf, int Rd, int Rn, int Rm);
void mc64_msub(struct mc64 *m, int sf, int Rd, int Rn, int Rm, int Ra);
void mc64_sdiv(struct mc64 *m, int sf, int Rd, int Rn, int Rm);
void mc64_udiv(struct mc64 *m, int sf, int Rd, int Rn, int Rm);
void mc64_lsl(struct mc64 *m, int sf, int Rd, int Rn, int Rm);   /* variable shift */
void mc64_asr(struct mc64 *m, int sf, int Rd, int Rn, int Rm);
void mc64_lsr(struct mc64 *m, int sf, int Rd, int Rn, int Rm);
void mc64_neg(struct mc64 *m, int sf, int Rd, int Rm);
void mc64_mvn(struct mc64 *m, int sf, int Rd, int Rm);
void mc64_sxtw(struct mc64 *m, int Rd, int Rn);      /* sign-extend Wn -> Xd */

/* ---- Compare and condition materialization. */
void mc64_cmp(struct mc64 *m, int sf, int Rn, int Rm);
void mc64_cset(struct mc64 *m, int sf, int Rd, int cc);

/* ---- Integer loads and stores, base+offset.  A sub-word load extends into the
   32-bit result; a signed load sign-extends.  The sink selects a valid offset
   encoding, materialising a large offset through a scratch base. */
void mc64_ldrb(struct mc64 *m, int Rt, int base, int off);
void mc64_ldrsb(struct mc64 *m, int Rt, int base, int off);
void mc64_ldrh(struct mc64 *m, int Rt, int base, int off);
void mc64_ldrsh(struct mc64 *m, int Rt, int base, int off);
void mc64_ldrw(struct mc64 *m, int Rt, int base, int off);
void mc64_ldrx(struct mc64 *m, int Rt, int base, int off);
void mc64_strb(struct mc64 *m, int Rt, int base, int off);
void mc64_strh(struct mc64 *m, int Rt, int base, int off);
void mc64_strw(struct mc64 *m, int Rt, int base, int off);
void mc64_strx(struct mc64 *m, int Rt, int base, int off);

/* ---- FP/SIMD scalar loads and stores.  sz: 1 = half, 2 = single, 3 = double. */
void mc64_ldr_fp(struct mc64 *m, int sz, int Rt, int base, int off);
void mc64_str_fp(struct mc64 *m, int sz, int Rt, int base, int off);

/* ---- FP data processing.  sz: 2 = single, 3 = double (1 = half where valid). */
void mc64_fmov(struct mc64 *m, int sz, int Rd, int Rn);
void mc64_fadd(struct mc64 *m, int sz, int Rd, int Rn, int Rm);
void mc64_fsub(struct mc64 *m, int sz, int Rd, int Rn, int Rm);
void mc64_fmul(struct mc64 *m, int sz, int Rd, int Rn, int Rm);
void mc64_fdiv(struct mc64 *m, int sz, int Rd, int Rn, int Rm);
void mc64_fneg(struct mc64 *m, int sz, int Rd, int Rn);
void mc64_fabs(struct mc64 *m, int sz, int Rd, int Rn);
void mc64_fsqrt(struct mc64 *m, int sz, int Rd, int Rn);
void mc64_fcmp(struct mc64 *m, int sz, int Rn, int Rm);
void mc64_fcvt(struct mc64 *m, int dstsz, int srcsz, int Rd, int Rn); /* between H/S/D */
void mc64_scvtf(struct mc64 *m, int sz, int Rd, int Rn);   /* Wn (int) -> Sd/Dd */
void mc64_fcvtzs(struct mc64 *m, int sz, int Rd, int Rn);  /* Sn/Dn -> Wd, toward zero */
void mc64_fcvtns(struct mc64 *m, int sz, int Rd, int Rn);  /* Sn/Dn -> Wd, to nearest */

/* ---- Call a named function (a module function or a runtime/host binding). */
void mc64_call_sym(struct mc64 *m, const char *name);

/* ---- The two arithmetic traps, each raises the fault and does not return. */
void mc64_trap_div_zero(struct mc64 *m);
void mc64_trap_overflow(struct mc64 *m);

/* ---- A source-line marker (the byte sink builds an address-to-line table). */
void mc64_loc(struct mc64 *m, int line);

/* ---- Inline assembly: emit a verbatim assembly string (text sink only; the
   byte sink reports it unsupported through its error channel). */
void mc64_asm(struct mc64 *m, const char *text);

#endif /* ARM64_MC_H */
