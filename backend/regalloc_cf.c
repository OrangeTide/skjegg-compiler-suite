/* regalloc_cf.c : linear-scan register allocator for ColdFire */
/*
 * Poletto & Sarkar (1999) linear-scan register allocation.
 *
 * ColdFire / m68k register parameters
 * ------------------------------------
 * Integer allocatable (callee-save): d2..d7  ->  6 regs, first = 2
 * Float   allocatable (callee-save): fp2..fp7 -> 6 regs, first = 2
 * I64 pair allocatable (callee-save): (d6,d7), (d4,d5), (d2,d3)  ->  3 pairs
 *
 * Reserved (integer):
 *   d0  return value / caller-save scratch
 *   d1  spill-fix-up scratch (reloads)
 *   a0  call lowering / address scratch
 *   a1  address scratch
 *   a6  frame pointer
 *   a7  stack pointer
 *
 * Reserved (float):
 *   fp0 scratch (float return / spill reload)
 *   fp1 scratch (second operand reload)
 *
 * The emitter inspects fn->temp_reg[t]:
 *   >= 0 -> physical register number (for I32/float)
 *           or pair index (for I64: 0=(d6,d7), 1=(d4,d5), 2=(d2,d3))
 *   -1   -> spilled; fn->temp_spill[t] is the byte offset
 *           within the spill area.
 *
 * I64 pairs are allocated from the top of d2..d7. I32 temps get whatever
 * remains from the bottom. This avoids overlap without a general graph
 * coloring allocator.
 *
 * fn->nspills    = integer spill slots (4 bytes each)
 * fn->nfspills   = float spill slots (8 bytes each)
 * fn->ni64spills = I64 spill slots (8 bytes each)
 */

#include "ir.h"

#include <stdlib.h>  /* qsort */

/* cf_emit.c: the allocator-index (temp_reg value) of each integer register an
   extended asm pins or clobbers; the allocator reserves them. */
int cf_asm_reserved_regs(struct ir_func *fn, int *out, int max);

#define INT_NUM_REGS  6
#define INT_FIRST_REG 2

#define FP_NUM_REGS   6
#define FP_FIRST_REG  2

#define I64_NUM_PAIRS  3
#define I64_FIRST_PAIR 0

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

/* reserved (or NULL): length num_regs, 1 = this pool slot is withheld from
   allocation (an extended-asm operand is pinned or clobbered there). */
static int
linscan(struct ir_func *fn, int *first_def, int *last_use,
        int *is_class, int num_regs, int first_reg, int spill_size,
        const int *reserved)
{
    struct interval *ivs;
    struct interval **active;
    struct arena_mark m;
    int ntemps = fn->ntemps;
    int niv;
    int nactive;
    int nspills;
    int t, k, p, r, w;
    /* A fixed array, not int pool[num_regs]: skj-cc compiles this file for the
       self-host build and has no VLAs.  num_regs is at most FP_NUM_REGS (the
       largest class any caller passes), so the array always covers it.  It is
       declared with the other locals, not mid-block, so the frame layout the
       self-host cf backend emits for it is the ordinary one. */
    int pool[FP_NUM_REGS];

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

    for (k = 0; k < num_regs; k++)
        pool[k] = (reserved && reserved[k]) ? 0 : 1;

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

void
regalloc(struct ir_func *fn)
{
    struct ir_insn *i;
    struct arena_mark m;
    int *first_def;
    int *last_use;
    int *is_float;
    int *is_i64;
    int *is_int;
    int ntemps;
    int pos;
    int t;
    int max_pair;
    int int_num_regs;

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
    is_i64 = arena_zalloc(fn->arena, ntemps * sizeof(int));
    is_int = arena_alloc(fn->arena, ntemps * sizeof(int));
    for (t = 0; t < ntemps; t++) {
        first_def[t] = -1;
        last_use[t] = -1;
    }

    /* cf_emit defers an argument's stack push from the IR_ARG to the consuming
       call (it records the arg temps and pushes them all at the call), so an
       IR_ARG operand is live until that call, not just until the ARG. Extend each
       pending argument's live range to its call, otherwise a temp defined between
       the ARG and the call (e.g. an IR_LEA entry address or another argument's
       constant) could be allocated the argument's register and clobber it before
       the push. Same latent gap the 64-bit backends close. */
    enum { RA_MAX_ARGS = 32 };      /* matches cf_emit.c's CF_MAX_ARGS cap */
    int pending[RA_MAX_ARGS];
    int npending = 0;

    pos = 0;
    for (i = fn->head; i; i = i->next, pos++) {
        if (i->dst >= 0 && i->dst < ntemps) {
            if (first_def[i->dst] < 0)
                first_def[i->dst] = pos;
            if (last_use[i->dst] < pos)
                last_use[i->dst] = pos;
            if (ir_op_is_float_def(i->op))
                is_float[i->dst] = 1;
            else if (ir_op_is_i64_def(i->op))
                is_i64[i->dst] = 1;
        }
        if (i->a >= 0 && i->a < ntemps && last_use[i->a] < pos)
            last_use[i->a] = pos;
        if (i->b >= 0 && i->b < ntemps && last_use[i->b] < pos)
            last_use[i->b] = pos;
        /* Extended inline asm: the operand temps live in asm_info, not in
           i->a/i->b.  Register an output as a def, an input as a use, an
           in-out operand as both, so two operands live at one asm never share
           a register. */
        if (i->op == IR_ASM && i->asm_info) {
            int k, nops = i->asm_info->noutputs + i->asm_info->ninputs;
            for (k = 0; k < nops; k++) {
                int t = i->asm_info->ops[k].temp;
                if (t < 0 || t >= ntemps)
                    continue;
                if ((i->asm_info->ops[k].is_output ||
                     i->asm_info->ops[k].is_inout) && first_def[t] < 0)
                    first_def[t] = pos;
                if (last_use[t] < pos)
                    last_use[t] = pos;
            }
        }

        if (i->op == IR_ARG || i->op == IR_FARG || i->op == IR_ARG64) {
            if (npending < RA_MAX_ARGS)
                pending[npending++] = i->a;
        } else if (i->op == IR_CALL || i->op == IR_CALLI ||
                   i->op == IR_FCALL || i->op == IR_FCALLI ||
                   i->op == IR_TAILCALL || i->op == IR_TAILCALLI ||
                   i->op == IR_CALL64 || i->op == IR_CALLI64) {
            int k;
            for (k = 0; k < npending; k++)
                if (pending[k] >= 0 && pending[k] < ntemps &&
                    last_use[pending[k]] < pos)
                    last_use[pending[k]] = pos;
            npending = 0;
        }
    }

    fn->ni64spills = linscan(fn, first_def, last_use,
                 is_i64, I64_NUM_PAIRS, I64_FIRST_PAIR, 8, NULL);

    max_pair = -1;
    for (t = 0; t < ntemps; t++) {
        if (is_i64[t] && fn->temp_reg[t] > max_pair)
            max_pair = fn->temp_reg[t];
    }
    int_num_regs = INT_NUM_REGS - 2 * (max_pair + 1);
    if (int_num_regs < 0)
        int_num_regs = 0;

    for (t = 0; t < ntemps; t++)
        is_int[t] = !is_float[t] && !is_i64[t];

    /* Reserve any integer register an extended-asm operand pins or clobbers,
       so no temp lives there across the asm.  A pin to the i64-pair region is
       unsupported (the pair pool is carved from the same registers), so a
       function that both pins an allocatable register and uses 64-bit integers
       is rejected rather than miscompiled. */
    int reserved[INT_NUM_REGS];
    int pinned[INT_NUM_REGS], npinned, pk, rk;
    for (rk = 0; rk < INT_NUM_REGS; rk++)
        reserved[rk] = 0;
    npinned = cf_asm_reserved_regs(fn, pinned, INT_NUM_REGS);
    for (pk = 0; pk < npinned; pk++) {
        int slot = pinned[pk] - INT_FIRST_REG;
        if (max_pair >= 0)
            die("cf: asm pin to an allocatable register in a function that "
                "uses 64-bit integers is not supported");
        if (slot >= 0 && slot < int_num_regs)
            reserved[slot] = 1;
    }

    fn->nspills = linscan(fn, first_def, last_use,
                  is_int, int_num_regs, INT_FIRST_REG, 4, reserved);
    fn->nfspills = linscan(fn, first_def, last_use,
                   is_float, FP_NUM_REGS, FP_FIRST_REG, 8, NULL);

    arena_release(fn->arena, m);
}
