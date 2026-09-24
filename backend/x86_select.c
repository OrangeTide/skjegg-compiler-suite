/* x86_select.c : the shared x86-64 instruction selector (see x86_select.h).

   Lifted from the JIT's emit_func: the register model, frame geometry, operand
   materialization, SysV call marshalling, prologue/epilogue, and the per-opcode
   switch, all expressed against the backend/mc.h sink.  The AOT (text) and JIT
   (byte) paths supply the sink; this selection logic is shared. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir.h"
#include "mc.h"
#include "x86_select.h"


/****************************************************************
 * Register model
 *
 * The allocator hands out five callee-saved integer registers by index;
 * map them to hardware here. The emitter keeps rax/rcx/rdx/r10/r11 as
 * within-instruction scratch (all caller-saved), and passes arguments in
 * the SysV order. The prologue saves and the epilogue restores all five
 * allocatable registers, so any assignment the allocator makes is safe.
 ****************************************************************/

static const int intreg[5] = { MC_RBX, MC_R12, MC_R13, MC_R14, MC_R15 };
static const int argreg[6] = { MC_RDI, MC_RSI, MC_RDX, MC_RCX, MC_R8, MC_R9 };

/* The FP allocator hands out xmm1..xmm7 (index 0..6); xmm0 and xmm8 are left
   free as float scratch. Float arguments and returns use xmm0..xmm7. */
static const int fpreg[7] = { MC_XMM1, MC_XMM2, MC_XMM3, MC_XMM4, MC_XMM5,
                              MC_XMM6, MC_XMM7 };
static const int fargreg[8] = { MC_XMM0, MC_XMM1, MC_XMM2, MC_XMM3,
                                MC_XMM4, MC_XMM5, MC_XMM6, MC_XMM7 };

#define SC0 MC_R10       /* integer scratch A */
#define SC1 MC_R11       /* integer scratch B */
#define FSC0 MC_XMM0     /* float scratch A */
#define FSC1 MC_XMM8     /* float scratch B */
#define SAVED 40        /* bytes of pushed callee-saved registers (5 * 8) */

/* Per-function frame geometry, all measured downward from rbp. */
struct frame {
    int *slot_off;      /* slot k's low byte sits at rbp - (SAVED + slot_off[k]) */
    int spill_base;     /* start of the integer spill area */
    int fspill_base;    /* start of the float spill area */
    int va_disp;        /* rbp displacement of the 176-byte SysV register save
                           area (variadic only; 0 otherwise) */
    int size;           /* bytes to subtract from rsp */
};


/* Signed condition-code for a compare opcode, or -1 if not a compare. */
static int
cc_of(int op)
{
    switch (op) {
    case IR_CMPEQ:  return MC_E;
    case IR_CMPNE:  return MC_NE;
    case IR_CMPLTS: return MC_L;
    case IR_CMPLES: return MC_LE;
    case IR_CMPGTS: return MC_G;
    case IR_CMPGES: return MC_GE;
    case IR_CMPLTU: return MC_B;
    case IR_CMPLEU: return MC_BE;
    case IR_CMPGTU: return MC_A;
    case IR_CMPGEU: return MC_AE;
    default: return -1;
    }
}

/* Stack displacement of slot k and of the spill home of temp t. */
static int
slot_disp(struct frame *fr, int k)
{
    return -(SAVED + fr->slot_off[k]);
}

static int
spill_disp(struct frame *fr, struct ir_func *fn, int t)
{
    return -(SAVED + fr->spill_base + fn->temp_spill[t] + 8);
}

static int
fspill_disp(struct frame *fr, struct ir_func *fn, int t)
{
    return -(SAVED + fr->fspill_base + fn->temp_spill[t] + 8);
}

/****************************************************************
 * Operand materialization
 ****************************************************************/

/* Load temp t (its full 64-bit home, which holds a value in the low 32 bits
   or a native address) into physical register `into`. */
static void
mat(struct mc *m, struct ir_func *fn, struct frame *fr, int t, int into)
{
    if (fn->temp_reg[t] >= 0) {
        int src = intreg[fn->temp_reg[t]];
        if (src != into)
            mc_mov_rr(m, into, src);
    } else {
        mc_load64(m, into, MC_RBP, spill_disp(fr, fn, t));
    }
}

/* Store physical register `from` back into temp t's home. */
static void
wb(struct mc *m, struct ir_func *fn, struct frame *fr, int t, int from)
{
    if (fn->temp_reg[t] >= 0) {
        int dst = intreg[fn->temp_reg[t]];
        if (dst != from)
            mc_mov_rr(m, dst, from);
    } else {
        mc_store64(m, MC_RBP, spill_disp(fr, fn, t), from);
    }
}

/* The float counterparts: a float temp lives in an xmm register from the FP
   pool or an 8-byte slot in the float spill area. */
static void
matf(struct mc *m, struct ir_func *fn, struct frame *fr, int t, int into)
{
    if (fn->temp_reg[t] >= 0) {
        int src = fpreg[fn->temp_reg[t]];
        if (src != into)
            mc_movsd_rr(m, into, src);
    } else {
        mc_movsd_load(m, into, MC_RBP, fspill_disp(fr, fn, t));
    }
}

static void
wbf(struct mc *m, struct ir_func *fn, struct frame *fr, int t, int from)
{
    if (fn->temp_reg[t] >= 0) {
        int dst = fpreg[fn->temp_reg[t]];
        if (dst != from)
            mc_movsd_rr(m, dst, from);
    } else {
        mc_movsd_store(m, MC_RBP, fspill_disp(fr, fn, t), from);
    }
}

/* ---- In-place operand model.
 *
 * mat/wb above always route a value through a fixed scratch register, which is
 * simple but costs a move per operand even when the temp already lives in a
 * register.  The helpers below instead name the temp's own register: a
 * register-allocated temp is read and written in place, and only a spilled temp
 * touches the scratch register (a reload for a source, a store-back for a
 * destination).  This is the shape the AOT's x86_emit.c used, and it keeps the
 * common register-to-register case at one or two instructions.
 *
 * A temp's home is 64 bits (an i32 in the low half, or a full i64/address), so
 * a reload and a store-back are always 64-bit, exactly as mat/wb do; only the
 * arithmetic that follows narrows to 32 bits.  An allocated integer register is
 * one of intreg[] (rbx/r12-r15), never a scratch (rax/rcx/rdx/r10/r11), so a
 * source register never collides with the SC0/SC1 scratch a sibling operand
 * reloads into. */

/* The register holding integer source temp t: its own register, or `scr` after
   reloading a spilled temp into it. */
static int
rs(struct mc *m, struct ir_func *fn, struct frame *fr, int t, int scr)
{
    if (fn->temp_reg[t] >= 0)
        return intreg[fn->temp_reg[t]];
    mc_load64(m, scr, MC_RBP, spill_disp(fr, fn, t));
    return scr;
}

/* The register to compute destination temp t into: its own register, or the
   scratch `scr`, which wd then stores back. */
static int
rd(struct ir_func *fn, int t, int scr)
{
    return fn->temp_reg[t] >= 0 ? intreg[fn->temp_reg[t]] : scr;
}

/* Store `reg` back into a spilled destination's home; a no-op for a temp that
   was computed in its own register. */
static void
wd(struct mc *m, struct ir_func *fn, struct frame *fr, int t, int reg)
{
    if (fn->temp_reg[t] < 0)
        mc_store64(m, MC_RBP, spill_disp(fr, fn, t), reg);
}

/* The float counterparts. */
static int
frs(struct mc *m, struct ir_func *fn, struct frame *fr, int t, int scr)
{
    if (fn->temp_reg[t] >= 0)
        return fpreg[fn->temp_reg[t]];
    mc_movsd_load(m, scr, MC_RBP, fspill_disp(fr, fn, t));
    return scr;
}

static int
frd(struct ir_func *fn, int t, int scr)
{
    return fn->temp_reg[t] >= 0 ? fpreg[fn->temp_reg[t]] : scr;
}

static void
fwd(struct mc *m, struct ir_func *fn, struct frame *fr, int t, int reg)
{
    if (fn->temp_reg[t] < 0)
        mc_movsd_store(m, MC_RBP, fspill_disp(fr, fn, t), reg);
}

/* Move the pending call arguments into their registers. Integer and float
   arguments consume separate register sequences under the SysV ABI. */
/* Marshal the pending call arguments into the SysV registers and, past the
   register capacity, onto the stack.  Returns the number of bytes reserved on
   the stack, which the caller pops after the call returns. */
static int
marshal(struct mc *m, struct ir_func *fn, struct frame *fr,
        int *pend_temp, char *pend_isf, char *pend_fw, int *pend_msize,
        int npending)
{
    int gi = 0, fi = 0, stack_bytes = 0, space, off;

    /* SysV passes the first six integer arguments in registers and the first
       eight float arguments in xmm registers; the rest go on the stack, in
       source order, the leftmost at the lowest address.  A MEMORY-class struct
       argument (pend_msize > 0) is always a stack copy, occupying its rounded
       byte size.  Count the stack bytes and reserve a 16-aligned block so rsp
       stays 16-aligned at the call (the body already runs with rsp 16-aligned). */
    for (int i = 0; i < npending; i++) {
        if (pend_msize[i] > 0)      stack_bytes += (pend_msize[i] + 7) & ~7;
        else if (pend_isf[i])     { if (fi++ >= 8) stack_bytes += 8; }
        else                      { if (gi++ >= 6) stack_bytes += 8; }
    }
    space = (stack_bytes + 15) & ~15;
    if (space)
        mc_sub_ri32(m, MC_RSP, space);

    /* Place the stack arguments first, through scratch registers that are not
       argument registers (r10, and xmm8 for floats), so loading the register
       arguments next cannot clobber a value already parked on the stack. */
    gi = fi = off = 0;
    for (int i = 0; i < npending; i++) {
        if (pend_msize[i] > 0) {
            /* copy the struct's bytes to [rsp+off]; the address is in the temp */
            int rem = pend_msize[i], o = 0;
            mat(m, fn, fr, pend_temp[i], SC1);   /* struct address */
            while (rem >= 8) {
                mc_load64(m, SC0, SC1, o);
                mc_store64(m, MC_RSP, off + o, SC0);
                o += 8; rem -= 8;
            }
            if (rem >= 4) {
                mc_load32(m, SC0, SC1, o);
                mc_store32(m, MC_RSP, off + o, SC0);
                o += 4; rem -= 4;
            }
            if (rem >= 2) {
                mc_load16(m, SC0, SC1, o);
                mc_store16(m, MC_RSP, off + o, SC0);
                o += 2; rem -= 2;
            }
            if (rem >= 1) {
                mc_load8(m, SC0, SC1, o);
                mc_store8(m, MC_RSP, off + o, SC0);
            }
            off += (pend_msize[i] + 7) & ~7;
        } else if (pend_isf[i]) {
            if (fi >= 8) {
                matf(m, fn, fr, pend_temp[i], FSC1);
                /* the value is already f32 in its register for a single arg, so
                   just store its natural width (4 bytes single, 8 double) */
                if (pend_fw[i])
                    mc_movss_store(m, MC_RSP, off, FSC1);
                else
                    mc_movsd_store(m, MC_RSP, off, FSC1);
                off += 8;
            }
            fi++;
        } else {
            if (gi >= 6) {
                mat(m, fn, fr, pend_temp[i], SC0);
                mc_store64(m, MC_RSP, off, SC0);
                off += 8;
            }
            gi++;
        }
    }

    /* Then load the register arguments.  Their sources are callee-saved
       registers or spill slots, never an argument register, so filling
       rdi..r9 / xmm0..7 in order does not clobber a not-yet-read source.  A
       MEMORY struct consumes no register, so it is skipped here. */
    gi = fi = 0;
    for (int i = 0; i < npending; i++) {
        if (pend_msize[i] > 0)
            continue;
        if (pend_isf[i]) {
            if (fi < 8)
                /* the value already holds f32 for a single arg, so a plain move
                   into the argument xmm suffices (no narrowing) */
                matf(m, fn, fr, pend_temp[i], fargreg[fi]);
            fi++;
        } else {
            if (gi < 6)
                mat(m, fn, fr, pend_temp[i], argreg[gi]);
            gi++;
        }
    }
    /* al = number of SSE registers used, which a variadic callee reads.  Set
       last so no later arg load clobbers it; a module (rel32) or indirect (r11)
       call preserves it, and an imported host function is never variadic. */
    mc_mov_ri32(m, MC_RAX, fi > 8 ? 8 : fi);
    return space;
}

/****************************************************************
 * Function emit
 ****************************************************************/

static void
prologue(struct mc *m, struct ir_func *fn, struct frame *fr)
{
    mc_push_r(m, MC_RBP);
    mc_mov_rr(m, MC_RBP, MC_RSP);
    mc_push_r(m, MC_RBX);
    mc_push_r(m, MC_R12);
    mc_push_r(m, MC_R13);
    mc_push_r(m, MC_R14);
    mc_push_r(m, MC_R15);
    if (fr->size)
        mc_sub_ri32(m, MC_RSP, fr->size);
    /* Spill each incoming parameter into its slot (slots 0..nparams-1 are the
       parameters), following the System V AMD64 classification so a struct
       parameter (1-2 eightbytes each INTEGER or SSE) is reconstructed
       contiguously in its slot.  Integer and SSE arguments consume separate
       register sequences; an argument past the register capacity, and a MEMORY
       (>16 byte) or register-classed struct whose eightbytes do not all fit,
       arrives on the incoming stack.  The caller placed the leftmost such
       eightbyte just above the return address, at [rbp+16], the next at
       [rbp+24], and so on.  Copy each into its slot through r10 / xmm8
       (scratch, not argument registers). */
    int int_used = 0, sse_used = 0, si = 0;
    for (int i = 0; i < fn->nparams; i++) {
        int base = slot_disp(fr, i);
        int sz = fn->slot_size ? fn->slot_size[i] : 4;
        int w8 = sz >= 8;               /* store 8 bytes (i64/ptr/struct) or 4 */
        int neb = fn->param_neb ? fn->param_neb[i] : 1;
        int mem = fn->param_cls && fn->param_cls[4 * i] == -2;
        int single = neb == 1 && fn->param_fw && fn->param_fw[i];
        int need_i = 0, need_s = 0, j;
        if (neb < 1)
            neb = 1;
        if (!mem)
            for (j = 0; j < neb; j++) {
                int cls = fn->param_cls ? fn->param_cls[4 * i + j] : 0;
                if (cls == 1) need_s++; else need_i++;
            }
        /* MEMORY, or eightbytes that do not all fit: read neb eightbytes from
           the incoming stack (all-or-nothing, matching the caller). */
        if (mem || int_used + need_i > 6 || sse_used + need_s > 8) {
            for (j = 0; j < neb; j++) {
                if (w8 || j > 0) {
                    mc_load64(m, SC0, MC_RBP, 16 + 8 * (si + j));
                    mc_store64(m, MC_RBP, base + 8 * j, SC0);
                } else {
                    mc_load32(m, SC0, MC_RBP, 16 + 8 * (si + j));
                    mc_store32(m, MC_RBP, base + 8 * j, SC0);
                }
            }
            si += neb;
            continue;
        }
        /* every eightbyte fits in registers: store each into base+8*j */
        for (j = 0; j < neb; j++) {
            int cls = fn->param_cls ? fn->param_cls[4 * i + j] : 0;
            if (cls == 1) {
                if (single)
                    mc_movss_store(m, MC_RBP, base + 8 * j, fargreg[sse_used++]);
                else
                    mc_movsd_store(m, MC_RBP, base + 8 * j, fargreg[sse_used++]);
            } else if (w8 || j > 0) {
                mc_store64(m, MC_RBP, base + 8 * j, argreg[int_used++]);
            } else {
                mc_store32(m, MC_RBP, base + 8 * j, argreg[int_used++]);
            }
        }
    }
    /* Variadic: save all six gp argument registers (48 bytes) then all eight
       xmm argument registers (16 bytes each) into the register save area, the
       backing __va_arg walks.  Named params were already spilled into their
       slots above; saving the registers again here is harmless (va_arg skips
       the named ones via gp_offset/fp_offset). */
    if (fn->is_variadic) {
        int k;
        for (k = 0; k < 6; k++)
            mc_store64(m, MC_RBP, fr->va_disp + 8 * k, argreg[k]);
        for (k = 0; k < 8; k++)
            mc_movsd_store(m, MC_RBP, fr->va_disp + 48 + 16 * k, fargreg[k]);
    }
}

static void
epilogue(struct mc *m, struct frame *fr)
{
    if (fr->size)
        mc_add_ri32(m, MC_RSP, fr->size);
    mc_pop_r(m, MC_R15);
    mc_pop_r(m, MC_R14);
    mc_pop_r(m, MC_R13);
    mc_pop_r(m, MC_R12);
    mc_pop_r(m, MC_RBX);
    mc_pop_r(m, MC_RBP);
    mc_ret(m);
}

/* Map an IR label number to its mc label id, allocating one on first use. */
static int
il(struct mc *m, int *lbl, int n)
{
    if (lbl[n] < 0)
        lbl[n] = mc_new_label(m);
    return lbl[n];
}

/* Emit one function's body through the mc sink.  Branch targets are mc labels
   the sink resolves; intra-module calls and code-pointer fixups are deferred by
   the sink's mc_call_sym / mc_lea_sym. */
int
x86_select_func(struct mc *m, struct ir_func *fn, int tp,
                char *err, size_t errlen)
{
    struct frame fr;
    int *lbl;               /* IR label number -> mc label id (allocated lazily) */
    int pend_temp[32], npending = 0;
    char pend_isf[32];
    char pend_fw[32];
    int pend_msize[32];
    char *is_float;
    int off, rc = 0;

    /* frame geometry */
    fr.slot_off = malloc((fn->nslots > 0 ? fn->nslots : 1) * sizeof(int));
    off = 0;
    for (int k = 0; k < fn->nslots; k++) {
        int sz = fn->slot_size ? fn->slot_size[k] : 4;
        if (sz <= 0)
            sz = 8;
        /* IR_LDL/IR_STL access a scalar slot four bytes wide (eight for an
           i64/pointer), so a sub-word slot (a char or short local, slot_size
           1 or 2) must still reserve the full access width, or a store spills
           into the next slot.  Reserve at least four bytes and keep the stride
           4-aligned; larger (aggregate) slots keep their own size. */
        if (sz < 4)
            sz = 4;
        sz = (sz + 3) & ~3;
        off += sz;
        fr.slot_off[k] = off;
    }
    fr.spill_base = off;
    fr.fspill_base = off + fn->nspills * 8;
    off += fn->nspills * 8 + fn->nfspills * 8;
    /* A variadic function reserves a 176-byte SysV register save area (6 gp
       registers of 8 bytes, then 8 xmm registers of 16 bytes) that the
       prologue fills and __va_arg walks. */
    fr.va_disp = 0;
    if (fn->is_variadic) {
        off += 176;
        fr.va_disp = -(SAVED + off);
    }
    fr.size = ((off + 15) & ~15) + 8;

    /* classify each temp exactly as the allocator did, so temp_reg is read
       against the right register file */
    is_float = calloc((size_t)(fn->ntemps > 0 ? fn->ntemps : 1), 1);
    for (struct ir_insn *in = fn->head; in; in = in->next)
        if (in->dst >= 0 && in->dst < fn->ntemps)
            is_float[in->dst] = (char)ir_op_is_float_def(in->op);

    lbl = malloc((size_t)(fn->nlabels > 0 ? fn->nlabels : 1) * sizeof(int));
    for (int k = 0; k < fn->nlabels; k++)
        lbl[k] = -1;

    prologue(m, fn, &fr);

    for (struct ir_insn *in = fn->head; in && rc == 0; in = in->next) {
        int cc = cc_of(in->op);
        if (cc >= 0) {
            int sa = rs(m, fn, &fr, in->a, SC0);
            int sb = rs(m, fn, &fr, in->b, SC1);
            int sd = rd(fn, in->dst, SC0);
            mc_cmp32_rr(m, sa, sb);      /* reads a, b before sd overwrites them */
            mc_setcc_r(m, cc, sd);
            mc_movzx_rb(m, sd, sd);
            wd(m, fn, &fr, in->dst, sd);
            continue;
        }
        switch (in->op) {
        case IR_LOC:
            /* A debug marker: no code, just note the line at this offset. */
            mc_loc(m, (int)in->imm);
            break;
        case IR_NOP:
        case IR_MARK:
        case IR_FUNC:   /* function-body delimiters skjegg's front ends emit; */
        case IR_ENDF:   /* the ir_func struct already frames the body here */
        case IR_LABEL:
            if (in->op == IR_LABEL && in->label >= 0)
                mc_label(m, il(m, lbl, in->label));
            break;
        case IR_LIC: {
            int sd = rd(fn, in->dst, SC0);
            mc_mov_ri32(m, sd, (uint32_t)in->imm);
            wd(m, fn, &fr, in->dst, sd);
            break;
        }
        case IR_LIC64: {       /* a 64-bit immediate: a set literal's bitmask */
            int sd = rd(fn, in->dst, SC0);
            mc_mov_ri64(m, sd, (uint64_t)in->imm);
            wd(m, fn, &fr, in->dst, sd);
            break;
        }
        case IR_LEA:
        case IR_LEA64: {
            /* the address of a data global or a function's code.  The 32-bit
               (ILP32) and 64-bit (LP64) forms select the same instruction: the
               sink loads the symbol's full address (mc_lea_sym), and the
               narrow-view write of an ILP32 result keeps only the low 32 bits. */
            int sd = rd(fn, in->dst, SC0);
            mc_lea_sym(m, sd, in->sym);
            wd(m, fn, &fr, in->dst, sd);
            break;
        }
        case IR_LB:            /* load one byte, zero-extended (a string character) */
        case IR_LBS:           /* load one byte, sign-extended (signed char) */
        case IR_LH:            /* load a halfword, zero-extended (unsigned short) */
        case IR_LHS:           /* load a halfword, sign-extended (short) */
        case IR_LW:
        case IR_LD64: {        /* load a 64-bit value (a set) from the address in a */
            int sa = rs(m, fn, &fr, in->a, SC0);
            int sd = rd(fn, in->dst, SC1);
            switch (in->op) {
            case IR_LB:   mc_load8(m, sd, sa, 0); break;
            case IR_LBS:  mc_load8s(m, sd, sa, 0); break;
            case IR_LH:   mc_load16(m, sd, sa, 0); break;
            case IR_LHS:  mc_load16s(m, sd, sa, 0); break;
            case IR_LW:   mc_load32(m, sd, sa, 0); break;
            default:      mc_load64(m, sd, sa, 0); break;   /* IR_LD64 */
            }
            wd(m, fn, &fr, in->dst, sd);
            break;
        }
        case IR_SB:            /* store the low byte of b to the address in a */
        case IR_SH:            /* store the low halfword of b to the address in a */
        case IR_SW:
        case IR_ST64: {        /* store the 64-bit value b to the address in a */
            int sa = rs(m, fn, &fr, in->a, SC0);
            int sb = rs(m, fn, &fr, in->b, SC1);
            switch (in->op) {
            case IR_SB:   mc_store8(m, sa, 0, sb); break;
            case IR_SH:   mc_store16(m, sa, 0, sb); break;
            case IR_SW:   mc_store32(m, sa, 0, sb); break;
            default:      mc_store64(m, sa, 0, sb); break;   /* IR_ST64 */
            }
            break;
        }
        case IR_LDL: {
            int sz = fn->slot_size ? fn->slot_size[in->slot] : 4;
            int sd = rd(fn, in->dst, SC0);
            if (sz == 8)
                mc_load64(m, sd, MC_RBP, slot_disp(&fr, in->slot));
            else
                mc_load32(m, sd, MC_RBP, slot_disp(&fr, in->slot));
            wd(m, fn, &fr, in->dst, sd);
            break;
        }
        case IR_STL: {
            int sz = fn->slot_size ? fn->slot_size[in->slot] : 4;
            int sa = rs(m, fn, &fr, in->a, SC0);
            if (sz == 8)
                mc_store64(m, MC_RBP, slot_disp(&fr, in->slot), sa);
            else
                mc_store32(m, MC_RBP, slot_disp(&fr, in->slot), sa);
            break;
        }
        case IR_LDL64: {       /* always 64-bit: a set or process slot */
            int sd = rd(fn, in->dst, SC0);
            mc_load64(m, sd, MC_RBP, slot_disp(&fr, in->slot));
            wd(m, fn, &fr, in->dst, sd);
            break;
        }
        case IR_STL64: {
            int sa = rs(m, fn, &fr, in->a, SC0);
            mc_store64(m, MC_RBP, slot_disp(&fr, in->slot), sa);
            break;
        }
        case IR_ADL:
        case IR_ADL64: {
            /* address of a local slot: a full 64-bit stack address. It lives
               in a 64-bit temp home and feeds 64-bit address arithmetic, so
               it is never truncated. The ILP32 (IR_ADL) and LP64 (IR_ADL64)
               forms are the same lea. */
            int sd = rd(fn, in->dst, SC0);
            mc_lea(m, sd, MC_RBP, slot_disp(&fr, in->slot));
            wd(m, fn, &fr, in->dst, sd);
            break;
        }
        case IR_MOV:
            if (is_float[in->dst]) {
                int sa = frs(m, fn, &fr, in->a, FSC0);
                int sd = frd(fn, in->dst, FSC0);
                if (sa != sd)
                    mc_movsd_rr(m, sd, sa);
                fwd(m, fn, &fr, in->dst, sd);
            } else {
                int sa = rs(m, fn, &fr, in->a, SC0);
                int sd = rd(fn, in->dst, SC0);
                if (sa != sd)
                    mc_mov_rr(m, sd, sa);
                wd(m, fn, &fr, in->dst, sd);
            }
            break;
        /* Address-capable arithmetic runs 64-bit so pointer math (a LEA
           result plus an offset, as in record-field and array addressing)
           is not truncated. For a plain i32 value the low 32 bits are still
           correct, and every value consumer reads only those. */
        /* The i32 and i64 arithmetic and bitwise ops are the same register
           operations at two widths.  The i32 forms MUST run 32-bit: an x86
           32-bit op zeroes the upper half of its destination, which keeps every
           i32 value's high 32 bits clear.  That invariant is load-bearing under
           the ILP32 address model, where an i32 result may next be an operand
           of a 64-bit address computation (base + index); a 64-bit i32 op can
           leave garbage in bits 32+ (a carry from ADD, a sign fill from NEG),
           and the address add would then read it.  The i64 forms run 64-bit. */
        case IR_ADD: case IR_SUB: case IR_MUL:
        case IR_AND: case IR_OR: case IR_XOR:
        case IR_ADD64: case IR_SUB64: case IR_MUL64:
        case IR_AND64: case IR_OR64: case IR_XOR64: {
            /* Two-address in place: compute into the destination register.  Set
               it to a first (a plain move, skipped when the destination already
               is a), then apply the op with b.  If b happens to share the
               destination register, park it in rcx before the move to a would
               overwrite it. */
            int sa = rs(m, fn, &fr, in->a, SC0);
            int sb = rs(m, fn, &fr, in->b, SC1);
            int sd = rd(fn, in->dst, SC0);
            if (sb == sd && sa != sd) {
                mc_mov_rr(m, MC_RCX, sb);
                sb = MC_RCX;
            }
            if (sa != sd)
                mc_mov_rr(m, sd, sa);
            switch (in->op) {
            case IR_ADD:   mc_add32_rr(m, sd, sb); break;
            case IR_ADD64: mc_add_rr(m, sd, sb); break;
            case IR_SUB:   mc_sub32_rr(m, sd, sb); break;
            case IR_SUB64: mc_sub_rr(m, sd, sb); break;
            case IR_MUL:   mc_imul32_rr(m, sd, sb); break;
            case IR_MUL64: mc_imul_rr(m, sd, sb); break;
            case IR_AND:   mc_and32_rr(m, sd, sb); break;
            case IR_AND64: mc_and_rr(m, sd, sb); break;
            case IR_OR:    mc_or32_rr(m, sd, sb); break;
            case IR_OR64:  mc_or_rr(m, sd, sb); break;
            case IR_XOR:   mc_xor32_rr(m, sd, sb); break;
            case IR_XOR64: mc_xor_rr(m, sd, sb); break;
            }
            wd(m, fn, &fr, in->dst, sd);
            break;
        }
        case IR_SHL: case IR_SHL64:
            /* i32 left shift runs 32-bit: shifting a 32-bit value left can push
               bits into 32+, and under ILP32 an address is a clean 32-bit value
               too, so a scaled index (index << n) must stay 32-bit clean.  The
               i64 form (set-element masks) shifts the full 64 bits. */
        case IR_SHRS: case IR_SHRU:
            /* right shifts fill from bit 31, so they must be 32-bit; they only
               ever apply to i32 values, never addresses */
        case IR_SHRS64: case IR_SHRU64: {
            /* the Int64 right shifts fill from bit 63, so they run 64-bit.
               The count must land in cl before a is moved into the destination,
               since the destination may itself be the count's register. */
            int sa = rs(m, fn, &fr, in->a, SC0);
            int sd = rd(fn, in->dst, SC0);
            int sb = rs(m, fn, &fr, in->b, MC_RCX);
            if (sb != MC_RCX)
                mc_mov_rr(m, MC_RCX, sb);
            if (sa != sd)
                mc_mov_rr(m, sd, sa);
            switch (in->op) {
            case IR_SHL:    mc_shl32_cl(m, sd); break;
            case IR_SHL64:  mc_shl_cl(m, sd); break;
            case IR_SHRS:   mc_sar32_cl(m, sd); break;
            case IR_SHRU:   mc_shr32_cl(m, sd); break;
            case IR_SHRS64: mc_sar_cl(m, sd); break;
            case IR_SHRU64: mc_shr_cl(m, sd); break;
            }
            wd(m, fn, &fr, in->dst, sd);
            break;
        }
        case IR_DIVS: case IR_MODS: {
            /* Under TRAP_GUARDED (JIT), guard the divisor: an idiv by zero would
               fault the host with SIGFPE, so trap DIV_ZERO instead.  Under
               TRAP_HARDWARE (AOT), the CPU #DE is the fault, so emit the bare
               idiv (matching the historical x86_emit.c). */
            mat(m, fn, &fr, in->a, MC_RAX);
            mat(m, fn, &fr, in->b, SC0);
            if (tp == TRAP_GUARDED) {
                int over = mc_new_label(m);
                mc_test32_rr(m, SC0, SC0);
                mc_jcc(m, MC_NE, over);
                mc_trap_div_zero(m);    /* never returns; raises the trap */
                mc_label(m, over);
            }
            mc_cdq(m);
            mc_idiv32_r(m, SC0);
            wb(m, fn, &fr, in->dst, in->op == IR_DIVS ? MC_RAX : MC_RDX);
            break;
        }
        case IR_DIVU: case IR_MODU: {
            mat(m, fn, &fr, in->a, MC_RAX);
            mat(m, fn, &fr, in->b, SC0);   /* SC0 is r10, not rdx: safe to zero rdx */
            if (tp == TRAP_GUARDED) {
                int over = mc_new_label(m);
                mc_test32_rr(m, SC0, SC0);
                mc_jcc(m, MC_NE, over);
                mc_trap_div_zero(m);
                mc_label(m, over);
            }
            mc_xor32_rr(m, MC_RDX, MC_RDX); /* zero edx for unsigned div (not cdq) */
            mc_div32_r(m, SC0);
            wb(m, fn, &fr, in->dst, in->op == IR_DIVU ? MC_RAX : MC_RDX);
            break;
        }
        case IR_DIVS64: case IR_MODS64: {
            mat(m, fn, &fr, in->a, MC_RAX);
            mat(m, fn, &fr, in->b, SC0);
            if (tp == TRAP_GUARDED) {
                int over = mc_new_label(m);
                mc_test_rr(m, SC0, SC0);  /* divisor is a full 64-bit value */
                mc_jcc(m, MC_NE, over);
                mc_trap_div_zero(m);
                mc_label(m, over);
            }
            mc_cqo(m);
            mc_idiv_r(m, SC0);
            wb(m, fn, &fr, in->dst, in->op == IR_DIVS64 ? MC_RAX : MC_RDX);
            break;
        }
        case IR_DIVU64: case IR_MODU64: {
            mat(m, fn, &fr, in->a, MC_RAX);
            mat(m, fn, &fr, in->b, SC0);
            if (tp == TRAP_GUARDED) {
                int over = mc_new_label(m);
                mc_test_rr(m, SC0, SC0);
                mc_jcc(m, MC_NE, over);
                mc_trap_div_zero(m);
                mc_label(m, over);
            }
            mc_xor32_rr(m, MC_RDX, MC_RDX); /* zero rdx (a 32-bit xor clears all 64) */
            mc_div_r(m, SC0);
            wb(m, fn, &fr, in->dst, in->op == IR_DIVU64 ? MC_RAX : MC_RDX);
            break;
        }
        case IR_ADDO: case IR_SUBO: case IR_MULO: {
            /* Checked arithmetic runs 32-bit so the CPU overflow flag reflects
               signed i32 overflow (the only width a Kobold integer takes).  The
               C front end never emits these, so under TRAP_HARDWARE (AOT) the
               overflow branch is dropped and the bare arithmetic remains. */
            mat(m, fn, &fr, in->a, SC0);
            mat(m, fn, &fr, in->b, SC1);
            switch (in->op) {
            case IR_ADDO: mc_add32_rr(m, SC0, SC1); break;
            case IR_SUBO: mc_sub32_rr(m, SC0, SC1); break;
            case IR_MULO: mc_imul32_rr(m, SC0, SC1); break;
            }
            if (tp == TRAP_GUARDED) {
                int ok = mc_new_label(m);
                mc_jcc(m, MC_NO, ok);   /* skip the trap when OF is clear */
                mc_trap_overflow(m);    /* never returns; raises OVERFLOW */
                mc_label(m, ok);
            }
            wb(m, fn, &fr, in->dst, SC0);
            break;
        }
        case IR_NEG: case IR_NEG64: {
            /* i32 negate runs 32-bit: a 64-bit negate sign-fills bits 32+, and
               that garbage would corrupt a later address computation. */
            int sa = rs(m, fn, &fr, in->a, SC0);
            int sd = rd(fn, in->dst, SC0);
            if (sa != sd)
                mc_mov_rr(m, sd, sa);
            if (in->op == IR_NEG64)
                mc_neg_r(m, sd);
            else
                mc_neg32_r(m, sd);
            wd(m, fn, &fr, in->dst, sd);
            break;
        }
        case IR_NOT: {
            /* i32 only (i64 ~ lowers to XOR64 with -1).  Run 32-bit: a 64-bit
               NOT would set bits 32+, dirtying the value for a later address use. */
            int sa = rs(m, fn, &fr, in->a, SC0);
            int sd = rd(fn, in->dst, SC0);
            if (sa != sd)
                mc_mov_rr(m, sd, sa);
            mc_not32_r(m, sd);
            wd(m, fn, &fr, in->dst, sd);
            break;
        }
        case IR_SEXT64: {
            /* widen a signed i32 to Int64: sign-extend the low 32 bits (movsxd
               both reads a and writes the destination, so no separate move) */
            int sa = rs(m, fn, &fr, in->a, SC0);
            int sd = rd(fn, in->dst, SC0);
            mc_movsxd_rr(m, sd, sa);
            wd(m, fn, &fr, in->dst, sd);
            break;
        }
        case IR_ZEXT64:
            /* widen an unsigned i32 to Int64: a 32-bit mov zeroes the upper half */
        case IR_TRUNC64: {
            /* narrow Int64 to i32: keep the low 32 bits, clearing the upper half
               so the result is a clean i32 value.  Both are a 32-bit mov that
               reads a and writes the destination. */
            int sa = rs(m, fn, &fr, in->a, SC0);
            int sd = rd(fn, in->dst, SC0);
            mc_mov32_rr(m, sd, sa);
            wd(m, fn, &fr, in->dst, sd);
            break;
        }
        case IR_JMP:
            mc_jmp(m, il(m, lbl, in->label));
            break;
        case IR_BZ: case IR_BNZ: {
            int sa = rs(m, fn, &fr, in->a, SC0);
            mc_test32_rr(m, sa, sa);
            mc_jcc(m, in->op == IR_BZ ? MC_E : MC_NE, il(m, lbl, in->label));
            break;
        }
        case IR_ARG: case IR_FARG: case IR_ARG64: case IR_ARG_MEM:
            if (npending < 32) {
                pend_temp[npending] = in->a;
                pend_isf[npending] = (char)(in->op == IR_FARG);
                pend_fw[npending] = (char)(in->op == IR_FARG && in->imm == FWIDTH_F32);
                /* IR_ARG_MEM carries the by-value struct byte size in imm */
                pend_msize[npending] = in->op == IR_ARG_MEM ? (int)in->imm : 0;
                npending++;
            }
            break;
        case IR_CALL: case IR_FCALL: case IR_CALL64: {
            int space = marshal(m, fn, &fr, pend_temp, pend_isf, pend_fw,
                                pend_msize, npending);
            npending = 0;
            mc_call_sym(m, in->sym);
            if (space)
                mc_add_ri32(m, MC_RSP, space);   /* pop the stack arguments */
            if (in->dst >= 0) {
                if (in->op == IR_FCALL)
                    /* the result is in xmm0 at its natural width (f32 for a
                       single, f64 for a double), matching the register model */
                    wbf(m, fn, &fr, in->dst, MC_XMM0);
                else
                    wb(m, fn, &fr, in->dst, MC_RAX);
            }
            break;
        }
        case IR_CALLI: case IR_FCALLI: case IR_CALLI64: {
            /* indirect call through a code pointer in the callee temp `a` */
            int space = marshal(m, fn, &fr, pend_temp, pend_isf, pend_fw,
                                pend_msize, npending);
            npending = 0;
            mat(m, fn, &fr, in->a, SC1);   /* pointer: not an argument reg */
            mc_call_reg(m, SC1);
            if (space)
                mc_add_ri32(m, MC_RSP, space);   /* pop the stack arguments */
            if (in->dst >= 0) {
                if (in->op == IR_FCALLI)
                    wbf(m, fn, &fr, in->dst, MC_XMM0);
                else
                    wb(m, fn, &fr, in->dst, MC_RAX);
            }
            break;
        }
        case IR_CALL_AGG: case IR_CALLI_AGG: {
            /* struct-returning call: make the call, then store the returned
               eightbytes (rax/rdx INTEGER, xmm0/xmm1 SSE, per the imm-encoded
               classes) into the destination slot.  imm: neb in bits 0-2, each
               slot's class in a 2-bit field at bits 3+2*j (0 int, 1 double, 2
               float single; x86-64 never produces the single class). */
            int space = marshal(m, fn, &fr, pend_temp, pend_isf, pend_fw,
                                pend_msize, npending);
            npending = 0;
            if (in->op == IR_CALLI_AGG) {
                mat(m, fn, &fr, in->a, SC1);   /* pointer: not an argument reg */
                mc_call_reg(m, SC1);
            } else {
                mc_call_sym(m, in->sym);
            }
            if (space)
                mc_add_ri32(m, MC_RSP, space);   /* pop the stack arguments */
            {
                int neb = (int)(in->imm & 7);
                int cls2[2] = { (int)((in->imm >> 3) & 3),
                                (int)((in->imm >> 5) & 3) };
                int iret[2] = { MC_RAX, MC_RDX };
                int sret[2] = { MC_XMM0, MC_XMM1 };
                int iidx = 0, sidx = 0, base = slot_disp(&fr, in->slot), j;
                for (j = 0; j < neb; j++) {
                    if (cls2[j] == 1)
                        mc_movsd_store(m, MC_RBP, base + 8 * j, sret[sidx++]);
                    else if (cls2[j] == 2)
                        mc_movss_store(m, MC_RBP, base + 8 * j, sret[sidx++]);
                    else
                        mc_store64(m, MC_RBP, base + 8 * j, iret[iidx++]);
                }
            }
            break;
        }
        case IR_FLD: {          /* load float from address in temp a; imm gives width */
            /* A single-float value stays f32 in the register (movss); the width
               is carried explicitly by IR_F32TOF64 when it must widen. */
            int sa = rs(m, fn, &fr, in->a, SC0);
            int sd = frd(fn, in->dst, FSC0);
            if (in->imm == FWIDTH_F32)
                mc_movss_load(m, sd, sa, 0);
            else
                mc_movsd_load(m, sd, sa, 0);
            fwd(m, fn, &fr, in->dst, sd);
            break;
        }
        case IR_FSD: {          /* store float value b to address in temp a; imm gives width */
            int sa = rs(m, fn, &fr, in->a, SC0);
            int sb = frs(m, fn, &fr, in->b, FSC0);
            if (in->imm == FWIDTH_F32)
                mc_movss_store(m, sa, 0, sb);
            else
                mc_movsd_store(m, sa, 0, sb);
            break;
        }
        case IR_FLS: {          /* load f32 from address in temp a, widen to f64 */
            int sa = rs(m, fn, &fr, in->a, SC0);
            int sd = frd(fn, in->dst, FSC0);
            mc_cvtss2sd_load(m, sd, sa, 0);
            fwd(m, fn, &fr, in->dst, sd);
            break;
        }
        case IR_FSS: {          /* narrow f64 value b to f32, store to address in temp a */
            int sa = rs(m, fn, &fr, in->a, SC0);
            int sb = frs(m, fn, &fr, in->b, FSC0);
            mc_cvtsd2ss_rr(m, FSC0, sb);   /* narrow into scratch, keeping b intact */
            mc_movss_store(m, sa, 0, FSC0);
            break;
        }
        case IR_F32TOF64: {     /* register f32 -> f64 */
            int sa = frs(m, fn, &fr, in->a, FSC0);
            int sd = frd(fn, in->dst, FSC0);
            mc_cvtss2sd_rr(m, sd, sa);
            fwd(m, fn, &fr, in->dst, sd);
            break;
        }
        case IR_F64TOF32: {     /* register f64 -> f32 */
            int sa = frs(m, fn, &fr, in->a, FSC0);
            int sd = frd(fn, in->dst, FSC0);
            mc_cvtsd2ss_rr(m, sd, sa);
            fwd(m, fn, &fr, in->dst, sd);
            break;
        }
        case IR_FLDL: {
            /* A single-float local lives in a 4-byte slot and stays f32 in the
               register (movss); a double is a plain 8-byte movsd. */
            int sd = frd(fn, in->dst, FSC0);
            if (in->imm == FWIDTH_F32)
                mc_movss_load(m, sd, MC_RBP, slot_disp(&fr, in->slot));
            else
                mc_movsd_load(m, sd, MC_RBP, slot_disp(&fr, in->slot));
            fwd(m, fn, &fr, in->dst, sd);
            break;
        }
        case IR_FSTL: {
            int sa = frs(m, fn, &fr, in->a, FSC0);
            if (in->imm == FWIDTH_F32)
                mc_movss_store(m, MC_RBP, slot_disp(&fr, in->slot), sa);
            else
                mc_movsd_store(m, MC_RBP, slot_disp(&fr, in->slot), sa);
            break;
        }
        case IR_FADD: case IR_FSUB: case IR_FMUL: case IR_FDIV: {
            /* The width tag selects the single (ss) or double (sd) form, so a
               float and a double arithmetic op each round at their precision.
               Two-address in place like the integer binops, parking b in the
               spare float scratch when it shares the destination register. */
            int f32 = in->imm == FWIDTH_F32;
            int sa = frs(m, fn, &fr, in->a, FSC0);
            int sb = frs(m, fn, &fr, in->b, FSC1);
            int sd = frd(fn, in->dst, FSC0);
            if (sb == sd && sa != sd) {
                mc_movsd_rr(m, FSC1, sb);
                sb = FSC1;
            }
            if (sa != sd)
                mc_movsd_rr(m, sd, sa);
            switch (in->op) {
            case IR_FADD: (f32 ? mc_addss_rr : mc_addsd_rr)(m, sd, sb); break;
            case IR_FSUB: (f32 ? mc_subss_rr : mc_subsd_rr)(m, sd, sb); break;
            case IR_FMUL: (f32 ? mc_mulss_rr : mc_mulsd_rr)(m, sd, sb); break;
            case IR_FDIV: (f32 ? mc_divss_rr : mc_divsd_rr)(m, sd, sb); break;
            }
            fwd(m, fn, &fr, in->dst, sd);
            break;
        }
        case IR_FCMPEQ: case IR_FCMPLT: case IR_FCMPLE: {
            /* ucomiss/ucomisd (by width) sets the flags like an unsigned compare
               of a to b; the boolean result is an integer temp. NaN (parity) is
               out of scope. */
            int fcc = in->op == IR_FCMPEQ ? MC_E
                    : in->op == IR_FCMPLT ? MC_B : MC_BE;
            int sa = frs(m, fn, &fr, in->a, FSC0);
            int sb = frs(m, fn, &fr, in->b, FSC1);
            int sd = rd(fn, in->dst, SC0);
            if (in->imm == FWIDTH_F32)
                mc_ucomiss_rr(m, sa, sb);
            else
                mc_ucomisd_rr(m, sa, sb);
            mc_setcc_r(m, fcc, sd);
            mc_movzx_rb(m, sd, sd);
            wd(m, fn, &fr, in->dst, sd);
            break;
        }
        case IR_ITOF: {         /* signed i32 to float; width selects ss/sd */
            int sa = rs(m, fn, &fr, in->a, SC0);
            int sd = frd(fn, in->dst, FSC0);
            if (in->imm == FWIDTH_F32)
                mc_cvtsi2ss(m, sd, sa);
            else
                mc_cvtsi2sd(m, sd, sa);
            fwd(m, fn, &fr, in->dst, sd);
            break;
        }
        case IR_FSQRT: {        /* f64 square root */
            int sa = frs(m, fn, &fr, in->a, FSC0);
            int sd = frd(fn, in->dst, FSC0);
            mc_sqrtsd_rr(m, sd, sa);
            fwd(m, fn, &fr, in->dst, sd);
            break;
        }
        case IR_FTOI: {         /* float -> i32, truncating toward zero; width selects ss/sd */
            int sa = frs(m, fn, &fr, in->a, FSC0);
            int sd = rd(fn, in->dst, SC0);
            if (in->imm == FWIDTH_F32)
                mc_cvttss2si(m, sd, sa);
            else
                mc_cvttsd2si(m, sd, sa);
            wd(m, fn, &fr, in->dst, sd);
            break;
        }
        case IR_FROUND: {       /* f64 -> i32, round to nearest (round) */
            int sa = frs(m, fn, &fr, in->a, FSC0);
            int sd = rd(fn, in->dst, SC0);
            mc_cvtsd2si(m, sd, sa);
            wd(m, fn, &fr, in->dst, sd);
            break;
        }
        case IR_FABS: {         /* clear the sign bit through a GPR (bit 31 f32, 63 f64) */
            int sa = frs(m, fn, &fr, in->a, FSC0);
            int sd = frd(fn, in->dst, FSC0);
            mc_movq_from_xmm(m, SC0, sa);
            mc_mov_ri64(m, SC1, in->imm == FWIDTH_F32
                       ? 0x7FFFFFFFULL : 0x7FFFFFFFFFFFFFFFULL);
            mc_and_rr(m, SC0, SC1);
            mc_movq_to_xmm(m, sd, SC0);
            fwd(m, fn, &fr, in->dst, sd);
            break;
        }
        case IR_FNEG: {         /* flip the sign bit through a GPR (bit 31 f32, 63 f64) */
            int sa = frs(m, fn, &fr, in->a, FSC0);
            int sd = frd(fn, in->dst, FSC0);
            mc_movq_from_xmm(m, SC0, sa);
            mc_mov_ri64(m, SC1, in->imm == FWIDTH_F32
                       ? 0x80000000ULL : 0x8000000000000000ULL);
            mc_xor_rr(m, SC0, SC1);
            mc_movq_to_xmm(m, sd, SC0);
            fwd(m, fn, &fr, in->dst, sd);
            break;
        }
        case IR_FLH:
            /* widen a half at [a] to a double through the softfloat helper.
               SysV: the 16-bit half is passed in edi, the single-precision bits
               come back in eax, then widen f32 -> f64.  The JIT has no binding
               for __skj_extendhfsf, so this path is AOT-only in practice. */
            mat(m, fn, &fr, in->a, SC0);
            mc_load16(m, MC_RDI, SC0, 0);      /* movzx edi, word [addr] */
            mc_call_sym(m, "__skj_extendhfsf");
            mc_movd_to_xmm(m, FSC0, MC_RAX);   /* single bits -> xmm */
            mc_cvtss2sd_rr(m, FSC0, FSC0);
            wbf(m, fn, &fr, in->dst, FSC0);
            break;
        case IR_FSH:
            /* narrow a double b to a half and store it at [a] through the
               softfloat helper (SysV: single bits in edi, half bits in eax).
               Reload the address after the call, since the call clobbers the
               scratch registers. */
            matf(m, fn, &fr, in->b, FSC0);
            mc_cvtsd2ss_rr(m, FSC0, FSC0);
            mc_movd_from_xmm(m, MC_RDI, FSC0); /* single bits -> arg */
            mc_call_sym(m, "__skj_truncsfhf");
            mat(m, fn, &fr, in->a, SC0);
            mc_store16(m, SC0, 0, MC_RAX);     /* mov word [addr], ax */
            break;
        case IR_ASM:
            /* basic inline assembly: emitted verbatim (AOT text sink only) */
            mc_asm(m, in->sym);
            break;
        case IR_FRETV:
            /* the value already holds f32 for a single result, so it lands in
               xmm0 at its natural width; no conversion */
            matf(m, fn, &fr, in->a, MC_XMM0);
            epilogue(m, &fr);
            break;
        case IR_VA_START: {
            /* fill the va_list at [temp a] from the named-param counts and the
               register save area.  ILP32 layout: gp_offset@0, fp_offset@4,
               overflow_arg_area@8, reg_save_area@12 (4-byte pointers). */
            int ng = 0, nf = 0, ns = 0, k, j;
            for (k = 0; k < fn->nparams; k++) {
                int neb = fn->param_neb ? fn->param_neb[k] : 1;
                int mem = fn->param_cls && fn->param_cls[4 * k] == -2;
                if (neb < 1)
                    neb = 1;
                if (mem) { ns += neb; continue; }
                for (j = 0; j < neb; j++) {
                    int cls = fn->param_cls ? fn->param_cls[4 * k + j] : 0;
                    if (cls == 1 && nf < 8) nf++;
                    else if (cls != 1 && ng < 6) ng++;
                    else ns++;
                }
            }
            mat(m, fn, &fr, in->a, SC1);   /* va_list address */
            mc_mov_ri32(m, SC0, (uint32_t)(8 * ng));
            mc_store32(m, SC1, 0, SC0);        /* gp_offset  */
            mc_mov_ri32(m, SC0, (uint32_t)(48 + 16 * nf));
            mc_store32(m, SC1, 4, SC0);        /* fp_offset  */
            /* overflow_arg_area = the first unnamed stack arg, at rbp+16+8*ns;
               reg_save_area = the 176-byte area at rbp+va_disp.  These are
               pointers, so their width and their va_list offset follow the
               target model: LP64 stores an 8-byte pointer (overflow@8,
               reg_save@16, the standard __va_list_tag), ILP32 a 4-byte one
               (overflow@8, reg_save@12), valid because the JIT stack is low
               memory and the address fits in 32 bits. */
#ifdef CC_LP64
            mc_lea(m, SC0, MC_RBP, 16 + 8 * ns);
            mc_store64(m, SC1, 8, SC0);
            mc_lea(m, SC0, MC_RBP, fr.va_disp);
            mc_store64(m, SC1, 16, SC0);
#else
            mc_lea(m, SC0, MC_RBP, 16 + 8 * ns);
            mc_store32(m, SC1, 8, SC0);
            mc_lea(m, SC0, MC_RBP, fr.va_disp);
            mc_store32(m, SC1, 12, SC0);
#endif
            break;
        }
        case IR_RET:
            epilogue(m, &fr);
            break;
        case IR_RETV: case IR_RETV64:
            mat(m, fn, &fr, in->a, MC_RAX);
            epilogue(m, &fr);
            break;
        case IR_RETV_AGG: {
            /* pack the struct at [temp a] into the return registers: each
               eightbyte j goes to rax/rdx (INTEGER) or xmm0/xmm1 (SSE) per
               fn->ret_cls. */
            int iret[2] = { MC_RAX, MC_RDX };
            int sret[2] = { MC_XMM0, MC_XMM1 };
            int iidx = 0, sidx = 0, j;
            mat(m, fn, &fr, in->a, SC1);   /* struct address */
            for (j = 0; j < fn->ret_neb; j++) {
                if (fn->ret_cls[j] == 1)
                    mc_movsd_load(m, sret[sidx++], SC1, 8 * j);
                else if (fn->ret_cls[j] == 2)
                    mc_movss_load(m, sret[sidx++], SC1, 8 * j);
                else
                    mc_load64(m, iret[iidx++], SC1, 8 * j);
            }
            epilogue(m, &fr);
            break;
        }
        case IR_CMP64EQ: case IR_CMP64NE:
        case IR_CMP64LTS: case IR_CMP64LES: case IR_CMP64GTS: case IR_CMP64GES:
        case IR_CMP64LTU: case IR_CMP64LEU: case IR_CMP64GTU: case IR_CMP64GEU: {
            /* a full 64-bit compare (Int64 relations, set equality/membership); the
               boolean result is an integer temp, like the 32-bit compares above */
            int cc64;
            switch (in->op) {
            case IR_CMP64EQ:  cc64 = MC_E;  break;
            case IR_CMP64NE:  cc64 = MC_NE; break;
            case IR_CMP64LTS: cc64 = MC_L;  break;
            case IR_CMP64LES: cc64 = MC_LE; break;
            case IR_CMP64GTS: cc64 = MC_G;  break;
            case IR_CMP64GES: cc64 = MC_GE; break;
            case IR_CMP64LTU: cc64 = MC_B;  break;
            case IR_CMP64LEU: cc64 = MC_BE; break;
            case IR_CMP64GTU: cc64 = MC_A;  break;
            default:          cc64 = MC_AE; break;   /* IR_CMP64GEU */
            }
            int sa = rs(m, fn, &fr, in->a, SC0);
            int sb = rs(m, fn, &fr, in->b, SC1);
            int sd = rd(fn, in->dst, SC0);
            mc_cmp_rr(m, sa, sb);        /* reads a, b before sd overwrites them */
            mc_setcc_r(m, cc64, sd);
            mc_movzx_rb(m, sd, sd);
            wd(m, fn, &fr, in->dst, sd);
            break;
        }
        default:
            snprintf(err, errlen, "unsupported opcode %d in '%s'",
                     in->op, fn->name);
            rc = -1;
            break;
        }
    }

    free(fr.slot_off);
    free(lbl);
    free(is_float);
    return rc;
}
