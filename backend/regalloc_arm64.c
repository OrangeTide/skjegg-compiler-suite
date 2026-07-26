/* regalloc_arm64.c : linear-scan register allocator for AArch64 */
/*
 * Poletto & Sarkar (1999) linear-scan register allocation.
 *
 * AArch64 register parameters
 * ---------------------------
 * Integer allocatable (callee-save): x19..x28  ->  10 regs, first = 2
 *
 * Because AArch64 is a 64-bit machine, a 64-bit `long long` value fits in a
 * single register (its x-view) rather than a register pair. So i32 and i64
 * temps share ONE integer class here, one register each; the emitter selects
 * the w-view (32-bit ops) or x-view (64-bit ops) from the opcode. There is no
 * separate i64 pair pool as on the 32-bit backends.
 *
 * Reserved:
 *   x9   spill-fix-up scratch (first operand reload)
 *   x10  spill-fix-up scratch (second operand reload)
 *   x15  address materialisation scratch (out-of-range frame offsets)
 *   x0   return value / first arg / scratch
 *   x29  frame pointer
 *   x30  link register
 *   sp   stack pointer
 *
 * The emitter inspects fn->temp_reg[t]:
 *   >= 0 -> physical register number (index into the emitter's reg table)
 *   -1   -> spilled; fn->temp_spill[t] is the byte offset within the spill
 *           area (8-byte slots).
 *
 * Every integer spill slot is 8 bytes so i32 and i64 spills are uniform.
 * Floating point is not yet supported on this backend (the emitter die()s on
 * any float opcode), so float temps are simply left unallocated.
 *
 * fn->nspills = integer spill slots (8 bytes each)
 */

#include "ir.h"

#include <stdlib.h>  /* qsort */

#define INT_NUM_REGS  10
#define INT_FIRST_REG 2

/* Float allocatable (callee-save low 64 bits): d8..d15 -> 8 regs, first = 2
 * (indices 0-1 are the emitter's float scratch d0/d1). */
#define FP_NUM_REGS   8
#define FP_FIRST_REG  2

struct interval {
    int temp;
    int start;
    int end;
    int reg;
    int spill;
};

static int
sort_by_start(const void *pa, const void *pb)
{
    const struct interval *a = pa;
    const struct interval *b = pb;

    if (a->start != b->start)
        return a->start - b->start;
    return a->end - b->end;
}

/* force_spill (may be NULL): temps that must not get a register (a float temp
   live across a call, since the backend does not preserve d8-d15). */
static int
linscan(struct ir_func *fn, int *first_def, int *last_use,
        int *is_class, int num_regs, int first_reg, int spill_size,
        int *force_spill)
{
    struct interval *ivs;
    struct interval **active;
    struct arena_mark m;
    int ntemps = fn->ntemps;
    int niv;
    int nactive;
    int nspills;
    int t, k, p, r, w;

    m = arena_save(fn->arena);
    ivs = arena_alloc(fn->arena, ntemps * sizeof(*ivs));
    niv = 0;
    for (t = 0; t < ntemps; t++) {
        if (!is_class[t] || first_def[t] < 0)
            continue;
        ivs[niv].temp = t;
        ivs[niv].start = first_def[t];
        ivs[niv].end = last_use[t] >= first_def[t]
                       ? last_use[t] : first_def[t];
        ivs[niv].reg = -1;
        ivs[niv].spill = -1;
        niv++;
    }

    if (niv == 0) {
        arena_release(fn->arena, m);
        return 0;
    }

    if (num_regs == 0) {
        for (p = 0; p < niv; p++) {
            fn->temp_reg[ivs[p].temp] = -1;
            fn->temp_spill[ivs[p].temp] = p * spill_size;
        }
        arena_release(fn->arena, m);
        return niv;
    }

    qsort(ivs, niv, sizeof(*ivs), sort_by_start);

    int pool[num_regs];
    for (k = 0; k < num_regs; k++)
        pool[k] = 1;

    active = arena_alloc(fn->arena, (num_regs + 1) * sizeof(*active));
    nactive = 0;
    nspills = 0;

    for (p = 0; p < niv; p++) {
        struct interval *iv = &ivs[p];
        int got, ins_at;

        w = 0;
        for (r = 0; r < nactive; r++) {
            if (active[r]->end < iv->start) {
                pool[active[r]->reg - first_reg] = 1;
            } else {
                active[w++] = active[r];
            }
        }
        nactive = w;

        if (force_spill && force_spill[iv->temp]) {
            iv->spill = nspills++ * spill_size;
            continue;
        }

        got = -1;
        for (k = 0; k < num_regs; k++) {
            if (pool[k]) {
                got = k + first_reg;
                pool[k] = 0;
                break;
            }
        }

        if (got >= 0) {
            iv->reg = got;
            ins_at = nactive;
            for (r = 0; r < nactive; r++) {
                if (active[r]->end > iv->end) {
                    ins_at = r;
                    break;
                }
            }
            for (r = nactive; r > ins_at; r--)
                active[r] = active[r - 1];
            active[ins_at] = iv;
            nactive++;
        } else {
            struct interval *sp = active[nactive - 1];
            if (sp->end > iv->end) {
                iv->reg = sp->reg;
                sp->reg = -1;
                sp->spill = nspills++ * spill_size;
                nactive--;
                ins_at = nactive;
                for (r = 0; r < nactive; r++) {
                    if (active[r]->end > iv->end) {
                        ins_at = r;
                        break;
                    }
                }
                for (r = nactive; r > ins_at; r--)
                    active[r] = active[r - 1];
                active[ins_at] = iv;
                nactive++;
            } else {
                iv->spill = nspills++ * spill_size;
            }
        }
    }

    for (p = 0; p < niv; p++) {
        fn->temp_reg[ivs[p].temp] = ivs[p].reg;
        fn->temp_spill[ivs[p].temp] = ivs[p].spill;
    }

    arena_release(fn->arena, m);
    return nspills;
}

/* mark each float temp whose live range strictly contains a call (it must
   spill: the backend does not preserve the float callee-saved regs across a
   call) */
static void
mark_float_cross(struct ir_func *fn, int *first_def, int *last_use,
                 int *is_float, int *call_pos, int ncall, int *out)
{
    int t, c;
    for (t = 0; t < fn->ntemps; t++) {
        out[t] = 0;
        if (!is_float[t] || first_def[t] < 0)
            continue;
        for (c = 0; c < ncall; c++)
            if (call_pos[c] > first_def[t] && call_pos[c] < last_use[t]) {
                out[t] = 1;
                break;
            }
    }
}

void
regalloc(struct ir_func *fn)
{
    struct ir_insn *i;
    struct arena_mark m;
    int *first_def;
    int *last_use;
    int *is_float;
    int *is_int;
    int ntemps;
    int pos;
    int t;

    ntemps = fn->ntemps;
    fn->temp_reg = arena_alloc(fn->arena,
                               ntemps > 0 ? ntemps * sizeof(int) : 1);
    fn->temp_spill = arena_alloc(fn->arena,
                                 ntemps > 0 ? ntemps * sizeof(int) : 1);
    for (t = 0; t < ntemps; t++) {
        fn->temp_reg[t] = -1;
        fn->temp_spill[t] = -1;
    }
    if (ntemps == 0) {
        fn->nspills = 0;
        fn->nfspills = 0;
        fn->ni64spills = 0;
        return;
    }

    m = arena_save(fn->arena);
    first_def = arena_alloc(fn->arena, ntemps * sizeof(int));
    last_use = arena_alloc(fn->arena, ntemps * sizeof(int));
    is_float = arena_zalloc(fn->arena, ntemps * sizeof(int));
    is_int = arena_alloc(fn->arena, ntemps * sizeof(int));
    for (t = 0; t < ntemps; t++) {
        first_def[t] = -1;
        last_use[t] = -1;
    }

    /*
     * An IR_ARG/IR_ARG64 operand is not consumed until the following call,
     * yet its only textual use is the ARG itself. If a temp is defined
     * between an ARG and its call (e.g. an IR_LEA supplying a CALLI's target),
     * naive liveness would free the ARG operand's register and let the new
     * temp reuse it, corrupting the pushed argument. So pending arg operands
     * have their live range extended to the consuming call. (The 32-bit
     * backends never hit this because i64 and i32 use disjoint register
     * pools; this backend shares one pool.)
     */
    int pending[16];
    int npending = 0;

    int ninsn = 0;
    for (i = fn->head; i; i = i->next)
        ninsn++;
    int *call_pos = arena_alloc(fn->arena, (ninsn > 0 ? ninsn : 1) * sizeof(int));
    int *float_cross = arena_zalloc(fn->arena, ntemps * sizeof(int));
    int ncall = 0;

    pos = 0;
    for (i = fn->head; i; i = i->next, pos++) {
        if (i->dst >= 0 && i->dst < ntemps) {
            if (first_def[i->dst] < 0)
                first_def[i->dst] = pos;
            if (last_use[i->dst] < pos)
                last_use[i->dst] = pos;
            if (ir_op_is_float_def(i->op))
                is_float[i->dst] = 1;
        }
        if (i->a >= 0 && i->a < ntemps && last_use[i->a] < pos)
            last_use[i->a] = pos;
        if (i->b >= 0 && i->b < ntemps && last_use[i->b] < pos)
            last_use[i->b] = pos;

        if (i->op == IR_ARG || i->op == IR_ARG64 || i->op == IR_ARG_X8 ||
            i->op == IR_ARG_MEM) {
            if (npending < 16)
                pending[npending++] = i->a;
        } else if (i->op == IR_CALL || i->op == IR_CALLI ||
                   i->op == IR_CALL64 || i->op == IR_CALLI64 ||
                   i->op == IR_FCALL || i->op == IR_FCALLI ||
                   i->op == IR_CALL_AGG || i->op == IR_CALLI_AGG ||
                   i->op == IR_TAILCALL || i->op == IR_TAILCALLI) {
            int k;
            for (k = 0; k < npending; k++)
                if (pending[k] >= 0 && pending[k] < ntemps &&
                    last_use[pending[k]] < pos)
                    last_use[pending[k]] = pos;
            npending = 0;
            call_pos[ncall++] = pos;
        }
    }

    /* One integer class covers both i32 and i64 (one register each). */
    for (t = 0; t < ntemps; t++)
        is_int[t] = !is_float[t];

    mark_float_cross(fn, first_def, last_use, is_float, call_pos, ncall,
                     float_cross);
    fn->nspills = linscan(fn, first_def, last_use,
                  is_int, INT_NUM_REGS, INT_FIRST_REG, 8, NULL);
#ifdef CC_PSABI
    /* the prologue saves d8-d15, so a float temp may keep a callee-saved
       register across a call: no force-spill needed (unlike the stack-
       convention build, which does not preserve d8-d15) */
    (void)float_cross;
    fn->nfspills = linscan(fn, first_def, last_use,
                   is_float, FP_NUM_REGS, FP_FIRST_REG, 8, NULL);
#else
    fn->nfspills = linscan(fn, first_def, last_use,
                   is_float, FP_NUM_REGS, FP_FIRST_REG, 8, float_cross);
#endif
    fn->ni64spills = 0;

    arena_release(fn->arena, m);
}
