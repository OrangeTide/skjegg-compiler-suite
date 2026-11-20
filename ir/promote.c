/* promote.c : scalar register promotion (see promote.h).

   Soundness rests on two facts.  A not-address-taken slot has no aliasing writes,
   because every alias needs IR_ADL (the slot-address op) first, so the slot is
   reached only by the IR_LDL / IR_STL that name it directly.  And memory and the
   single promoted temp are both one location updated at the same program points,
   so they always hold the same value for a read that any store reaches, which is
   every read in a defined program.  The single-interval liveness keeps the temp
   in one place across all its defs and uses, so multiple stores (even in one
   block, or on separate paths into a join) stay correct: last writer wins, as it
   did in memory.

   Parameter home slots [0, nparams) are excluded: every backend's prologue fills
   them outside the IR, so a promoted temp would be undefined at its first use.

   Three slot shapes are promoted.  A word scalar touched only by IR_LDL / IR_STL.
   An 8-byte pointer, long, or i64 touched only by IR_LDL64 / IR_STL64; it becomes
   a single register on LP64 and a register pair on ILP32, both via IR_MOV64.  And a
   sub-word scalar (char / short): the store is still a word IR_STL, but each read
   is an IR_ADL / IR_ADL64 taking the slot address, used exactly once as the base
   of a sign/zero-extending load (IR_LBS / IR_LB / IR_LHS / IR_LH).  That slot is
   promoted to a word temp holding the stored value, each store becomes a move
   into it, each address op becomes a nop, and each load becomes a register
   sub-word extension (IR_SEXTB / IR_ZEXTB / IR_SEXTH / IR_ZEXTH) of that temp. */

#include <stdlib.h>

#include "ir.h"
#include "promote.h"

static int
is_subword_load(int op)
{
    return op == IR_LBS || op == IR_LB || op == IR_LHS || op == IR_LH;
}

/* The register extension that a sub-word load turns into. */
static int
load_to_ext(int op)
{
    switch (op) {
    case IR_LBS: return IR_SEXTB;
    case IR_LB:  return IR_ZEXTB;
    case IR_LHS: return IR_SEXTH;
    case IR_LH:  return IR_ZEXTH;
    default:     return -1;
    }
}

void
ir_promote_slots(struct ir_func *fn, int promote_wide)
{
    struct ir_insn *in;
    int ns = fn->nslots;
    int nt;
    /* per-slot tallies */
    int *n_stl, *n_ldl, *n_wide, *n_other, *n_adl_ok, *n_adl_bad, *subkind;
    int *store_seen;                             /* a store to the slot seen yet (textual) */
    /* per-temp: use count (as operand) and, when used once as `a`, the user */
    int *use_n;
    struct ir_insn **a_user;
    int *tmp;
    int s, any = 0;

    /* On by default.  SKJ_NO_PROMOTE disables it, as an escape hatch for
       measurement, A/B comparison, and bisecting a suspected promotion bug. */
    if (getenv("SKJ_NO_PROMOTE") || ns <= 0 || !fn->slot_size)
        return;

    nt = fn->ntemps;

    /* temp use map */
    use_n = arena_zalloc(fn->arena, (size_t)(nt > 0 ? nt : 1) * sizeof(int));
    a_user = arena_zalloc(fn->arena, (size_t)(nt > 0 ? nt : 1) * sizeof(*a_user));
    for (in = fn->head; in; in = in->next) {
        if (in->a >= 0 && in->a < nt) {
            use_n[in->a]++;
            a_user[in->a] = in;
        }
        if (in->b >= 0 && in->b < nt)
            use_n[in->b]++;
        if (in->op == IR_ASM && in->asm_info) {
            int k, nops = in->asm_info->noutputs + in->asm_info->ninputs;
            for (k = 0; k < nops; k++) {
                int at = in->asm_info->ops[k].temp;
                if (at >= 0 && at < nt)
                    use_n[at] += 2;              /* force out of the single-use class */
            }
        }
    }

    n_stl = arena_zalloc(fn->arena, (size_t)ns * sizeof(int));
    n_ldl = arena_zalloc(fn->arena, (size_t)ns * sizeof(int));
    n_wide = arena_zalloc(fn->arena, (size_t)ns * sizeof(int));
    n_other = arena_zalloc(fn->arena, (size_t)ns * sizeof(int));
    n_adl_ok = arena_zalloc(fn->arena, (size_t)ns * sizeof(int));
    n_adl_bad = arena_zalloc(fn->arena, (size_t)ns * sizeof(int));
    store_seen = arena_zalloc(fn->arena, (size_t)ns * sizeof(int));
    subkind = arena_alloc(fn->arena, (size_t)ns * sizeof(int));
    for (s = 0; s < ns; s++)
        subkind[s] = -1;

    for (in = fn->head; in; in = in->next) {
        int sl = in->slot;
        if (sl < 0 || sl >= ns)
            continue;
        if (sl < fn->nparams) {                  /* param home: prologue-filled */
            n_other[sl]++;
            continue;
        }
        switch (in->op) {
        /* Two disqualifiers guard every load and store.  First, a store's
           source and a load's destination must be real temps: a malformed
           module (the reader accepts some) can carry a slotted load or store
           with no temp operand, and promoting it would manufacture an IR_MOV
           with an out-of-range operand.  Second, a load that runs before any
           store to the slot reads the slot's entry value, which is set outside
           the IR (a managed reference is nil at entry and read-then-released on
           its first assignment).  A promoted register has no such entry value,
           so a read-before-write slot must stay in memory, exactly like a
           parameter home. */
        case IR_STL:
            if (in->a < 0 || in->a >= nt) { n_other[sl]++; break; }
            store_seen[sl] = 1;
            n_stl[sl]++;
            break;
        case IR_LDL:
            if (in->dst < 0 || in->dst >= nt || !store_seen[sl]) {
                n_other[sl]++; break;
            }
            n_ldl[sl]++;
            break;
        case IR_STL64:
            if (in->a < 0 || in->a >= nt) { n_other[sl]++; break; }
            store_seen[sl] = 1;
            n_wide[sl]++;
            break;
        case IR_LDL64:
            if (in->dst < 0 || in->dst >= nt || !store_seen[sl]) {
                n_other[sl]++; break;
            }
            n_wide[sl]++;
            break;
        case IR_ADL:
        case IR_ADL64: {
            int t = in->dst, ld;
            if (!store_seen[sl] || t < 0 || t >= nt || use_n[t] != 1 ||
                !a_user[t] || a_user[t]->a != t ||
                !is_subword_load(a_user[t]->op) ||
                a_user[t]->dst < 0 || a_user[t]->dst >= nt) {
                n_adl_bad[sl]++;                  /* escapes, not the idiom, bad dst, or rbw */
                break;
            }
            ld = a_user[t]->op;
            if (subkind[sl] == -1)
                subkind[sl] = ld;
            if (subkind[sl] != ld)
                n_adl_bad[sl]++;                  /* mixed sub-word types */
            else
                n_adl_ok[sl]++;
            break;
        }
        default:
            n_other[sl]++;                        /* float / aggregate / unknown */
            break;
        }
    }

    tmp = arena_alloc(fn->arena, (size_t)ns * sizeof(int));
    for (s = 0; s < ns; s++) {
        int word = n_stl[s] + n_ldl[s] > 0 && n_wide[s] == 0 &&
                   n_other[s] == 0 && n_adl_ok[s] == 0 && n_adl_bad[s] == 0 &&
                   fn->slot_size[s] == 4;
        int wide = promote_wide && n_wide[s] > 0 && n_stl[s] == 0 &&
                   n_ldl[s] == 0 && n_other[s] == 0 && n_adl_ok[s] == 0 &&
                   n_adl_bad[s] == 0 && fn->slot_size[s] == 8;
        int sub = n_adl_ok[s] > 0 && n_adl_bad[s] == 0 && n_other[s] == 0 &&
                  n_ldl[s] == 0 && n_wide[s] == 0 && n_stl[s] > 0 &&
                  fn->slot_size[s] <= 4;

        tmp[s] = -1;
        if (word || sub || wide) {
            tmp[s] = ir_new_temp(fn);        /* wide is a single reg on LP64, a
                                                register pair on ILP32 (IR_MOV64) */
            any = 1;
        }
    }
    if (!any)
        return;

    for (in = fn->head; in; in = in->next) {
        int sl = in->slot;
        if (sl < 0 || sl >= ns || tmp[sl] < 0)
            continue;
        switch (in->op) {
        case IR_STL:                             /* [slot] <- a   ==>  Ts <- a */
            in->op = IR_MOV;
            in->dst = tmp[sl];
            in->slot = -1;
            break;
        case IR_LDL:                             /* dst <- [slot]  ==>  dst <- Ts */
            in->op = IR_MOV;
            in->a = tmp[sl];
            in->slot = -1;
            break;
        case IR_STL64:                           /* 64-bit store ==> 64-bit move in */
            in->op = IR_MOV64;
            in->dst = tmp[sl];
            in->slot = -1;
            break;
        case IR_LDL64:                            /* 64-bit load  ==> 64-bit move out */
            in->op = IR_MOV64;
            in->a = tmp[sl];
            in->slot = -1;
            break;
        case IR_ADL:
        case IR_ADL64: {
            /* The read idiom: this address op feeds one sub-word load.  Rewrite
               that load into a register extension of the promoted temp, and drop
               the address op. */
            struct ir_insn *ld = a_user[in->dst];
            ld->op = load_to_ext(ld->op);
            ld->a = tmp[sl];
            in->op = IR_NOP;
            in->dst = -1;
            in->a = -1;
            in->slot = -1;
            break;
        }
        default:
            break;
        }
    }
}
