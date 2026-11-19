/* liveness.c : control-flow-aware live-interval computation (Phase A) */
/*
 * One shared pass, used by every backend's linear-scan allocator in place of
 * the old textual last_use scan.  Builds a control-flow graph from the flat
 * instruction list, solves liveness by backward dataflow, and turns the
 * live-out sets into single [first_def, last_use] intervals.  See
 * doc/internals/liveness.md for the why.
 *
 * Phase A only: no dominance.  first_def is held at the real definition
 * position, so a conditionally-defined temp cannot pull its interval back to
 * the entry; over-long intervals are always safe, they only spill.
 */
#include "ir.h"
#include "liveness.h"

/* ---- small bitset over temps -------------------------------------------- */

static void
bs_set(unsigned *s, int i)
{
    s[i >> 5] |= 1u << (i & 31);
}

static int
bs_get(const unsigned *s, int i)
{
    return (s[i >> 5] >> (i & 31)) & 1;
}

/* ---- terminators -------------------------------------------------------- */

static int
is_terminator(const struct ir_insn *in)
{
    switch (in->op) {
    case IR_JMP:
    case IR_BZ:
    case IR_BNZ:
    case IR_RET:
    case IR_RETV:
    case IR_RETV64:
    case IR_RETV_AGG:
    case IR_RETV2:
    case IR_TAILCALL:
    case IR_TAILCALLI:
    case IR_IJMP:
    case IR_IJMP64:
        return 1;
    case IR_ADDO:
    case IR_SUBO:
    case IR_MULO:
        /* overflow-checked arithmetic branches to label only when set */
        return in->label >= 0;
    default:
        return 0;
    }
}

/* ---- the pass ----------------------------------------------------------- */

void
ir_live_intervals(struct ir_func *fn, int *first_def, int *last_use)
{
    struct ir_insn *in;
    struct ir_insn **insns;
    int ntemps = fn->ntemps;
    int nlabels = fn->nlabels;
    int ninsn, words, nblocks;
    int *block_of, *leader, *blk_end, *label_pos;
    unsigned *use_s, *def_s, *in_s, *out_s;
    int *at_blocks, n_at = 0;
    unsigned *at_seen;
    int pos, t, b, k, changed;

    for (t = 0; t < ntemps; t++) {
        first_def[t] = -1;
        last_use[t] = -1;
    }
    if (ntemps == 0)
        return;

    /* count instructions and index them by position */
    ninsn = 0;
    for (in = fn->head; in; in = in->next)
        ninsn++;
    if (ninsn == 0)
        return;

    insns = arena_alloc(fn->arena, (size_t)ninsn * sizeof(*insns));
    pos = 0;
    for (in = fn->head; in; in = in->next)
        insns[pos++] = in;

    /* label id -> position of its IR_LABEL */
    label_pos = arena_alloc(fn->arena,
                            (size_t)(nlabels > 0 ? nlabels : 1) * sizeof(int));
    for (k = 0; k < nlabels; k++)
        label_pos[k] = -1;
    for (pos = 0; pos < ninsn; pos++) {
        in = insns[pos];
        if (in->op == IR_LABEL && in->label >= 0 && in->label < nlabels)
            label_pos[in->label] = pos;
    }

    /* leaders: entry, every label, every instruction after a terminator */
    leader = arena_zalloc(fn->arena, (size_t)ninsn * sizeof(int));
    leader[0] = 1;
    for (pos = 0; pos < ninsn; pos++) {
        in = insns[pos];
        if (in->op == IR_LABEL)
            leader[pos] = 1;
        if (is_terminator(in) && pos + 1 < ninsn)
            leader[pos + 1] = 1;
    }

    /* assign block ids, record each block's last position */
    block_of = arena_alloc(fn->arena, (size_t)ninsn * sizeof(int));
    nblocks = 0;
    for (pos = 0; pos < ninsn; pos++) {
        if (leader[pos])
            nblocks++;
        block_of[pos] = nblocks - 1;
    }
    blk_end = arena_alloc(fn->arena, (size_t)nblocks * sizeof(int));
    for (pos = 0; pos < ninsn; pos++)
        blk_end[block_of[pos]] = pos;

    /* address-taken labels -> unique list of their target blocks (IJMP edges) */
    at_blocks = arena_alloc(fn->arena, (size_t)nblocks * sizeof(int));
    at_seen = arena_zalloc(fn->arena,
                           (size_t)((nblocks + 31) / 32) * sizeof(unsigned));
    for (pos = 0; pos < ninsn; pos++) {
        in = insns[pos];
        if ((in->op == IR_LABADDR || in->op == IR_LABADDR64) &&
            in->label >= 0 && in->label < nlabels &&
            label_pos[in->label] >= 0) {
            int tb = block_of[label_pos[in->label]];
            if (!bs_get(at_seen, tb)) {
                bs_set(at_seen, tb);
                at_blocks[n_at++] = tb;
            }
        }
    }

    /* per-block use (upward-exposed) and def sets */
    words = (ntemps + 31) / 32;
    use_s = arena_zalloc(fn->arena, (size_t)nblocks * words * sizeof(unsigned));
    def_s = arena_zalloc(fn->arena, (size_t)nblocks * words * sizeof(unsigned));
    in_s = arena_zalloc(fn->arena, (size_t)nblocks * words * sizeof(unsigned));
    out_s = arena_zalloc(fn->arena, (size_t)nblocks * words * sizeof(unsigned));

    for (pos = 0; pos < ninsn; pos++) {
        unsigned *u, *d;
        in = insns[pos];
        b = block_of[pos];
        u = use_s + (size_t)b * words;
        d = def_s + (size_t)b * words;

        /* uses first: a and b are upward-exposed if not already defined in
         * this block; this matches the backend's textual operand scan */
        if (in->a >= 0 && in->a < ntemps && !bs_get(d, in->a))
            bs_set(u, in->a);
        if (in->b >= 0 && in->b < ntemps && !bs_get(d, in->b))
            bs_set(u, in->b);
        if (in->op == IR_ASM && in->asm_info) {
            int nops = in->asm_info->noutputs + in->asm_info->ninputs;
            for (k = 0; k < nops; k++) {
                int at = in->asm_info->ops[k].temp;
                if (at < 0 || at >= ntemps)
                    continue;
                if (!bs_get(d, at))
                    bs_set(u, at);
            }
        }
        /* then defs */
        if (in->dst >= 0 && in->dst < ntemps)
            bs_set(d, in->dst);
        if (in->op == IR_ASM && in->asm_info) {
            int nops = in->asm_info->noutputs + in->asm_info->ninputs;
            for (k = 0; k < nops; k++) {
                struct ir_asm_op *op = &in->asm_info->ops[k];
                if (op->temp >= 0 && op->temp < ntemps &&
                    (op->is_output || op->is_inout))
                    bs_set(d, op->temp);
            }
        }
    }

    /* backward dataflow to a fixed point:
     *   out[b] = union of in[succ];  in[b] = use[b] | (out[b] & ~def[b]) */
    do {
        changed = 0;
        for (b = nblocks - 1; b >= 0; b--) {
            unsigned *out = out_s + (size_t)b * words;
            unsigned *inb = in_s + (size_t)b * words;
            const unsigned *u = use_s + (size_t)b * words;
            const unsigned *d = def_s + (size_t)b * words;
            struct ir_insn *last = insns[blk_end[b]];
            int w;

            /* out[b] = union of in[] over successors */
            for (w = 0; w < words; w++)
                out[w] = 0;
            switch (last->op) {
            case IR_RET:
            case IR_RETV:
            case IR_RETV64:
            case IR_RETV_AGG:
            case IR_RETV2:
            case IR_TAILCALL:
            case IR_TAILCALLI:
                break;                        /* no successor */
            case IR_JMP:
                if (last->label >= 0 && last->label < nlabels &&
                    label_pos[last->label] >= 0) {
                    int s = block_of[label_pos[last->label]];
                    const unsigned *si = in_s + (size_t)s * words;
                    for (w = 0; w < words; w++)
                        out[w] |= si[w];
                }
                break;
            case IR_IJMP:
            case IR_IJMP64:
                for (k = 0; k < n_at; k++) {
                    const unsigned *si = in_s + (size_t)at_blocks[k] * words;
                    for (w = 0; w < words; w++)
                        out[w] |= si[w];
                }
                break;
            default:
                /* conditional branch (BZ/BNZ, overflow) or a block that ends
                 * only because the next instruction is a label: taken target
                 * if any, plus the fall-through */
                if ((last->op == IR_BZ || last->op == IR_BNZ ||
                     last->op == IR_ADDO || last->op == IR_SUBO ||
                     last->op == IR_MULO) &&
                    last->label >= 0 && last->label < nlabels &&
                    label_pos[last->label] >= 0) {
                    int s = block_of[label_pos[last->label]];
                    const unsigned *si = in_s + (size_t)s * words;
                    for (w = 0; w < words; w++)
                        out[w] |= si[w];
                }
                if (b + 1 < nblocks) {
                    const unsigned *si = in_s + (size_t)(b + 1) * words;
                    for (w = 0; w < words; w++)
                        out[w] |= si[w];
                }
                break;
            }

            /* in[b] = use[b] | (out[b] & ~def[b]) */
            for (w = 0; w < words; w++) {
                unsigned nv = u[w] | (out[w] & ~d[w]);
                if (nv != inb[w]) {
                    inb[w] = nv;
                    changed = 1;
                }
            }
        }
    } while (changed);

    /* textual first_def / last_use, mirroring the backend operand scan but
     * without the IR_ARG to call extension (the backend layers that on) */
    for (pos = 0; pos < ninsn; pos++) {
        in = insns[pos];
        if (in->dst >= 0 && in->dst < ntemps) {
            if (first_def[in->dst] < 0)
                first_def[in->dst] = pos;
            if (last_use[in->dst] < pos)
                last_use[in->dst] = pos;
        }
        if (in->a >= 0 && in->a < ntemps && last_use[in->a] < pos)
            last_use[in->a] = pos;
        if (in->b >= 0 && in->b < ntemps && last_use[in->b] < pos)
            last_use[in->b] = pos;
        if (in->op == IR_ASM && in->asm_info) {
            int nops = in->asm_info->noutputs + in->asm_info->ninputs;
            for (k = 0; k < nops; k++) {
                struct ir_asm_op *op = &in->asm_info->ops[k];
                int at = op->temp;
                if (at < 0 || at >= ntemps)
                    continue;
                if ((op->is_output || op->is_inout) && first_def[at] < 0)
                    first_def[at] = pos;
                if (last_use[at] < pos)
                    last_use[at] = pos;
            }
        }
    }

    /* control-flow extension: a temp live-out of a block is live at least to
     * that block's last instruction.  This is the back-edge fix. */
    for (b = 0; b < nblocks; b++) {
        const unsigned *out = out_s + (size_t)b * words;
        int end = blk_end[b];
        for (t = 0; t < ntemps; t++) {
            if (first_def[t] >= 0 && bs_get(out, t) && last_use[t] < end)
                last_use[t] = end;
        }
    }
}
