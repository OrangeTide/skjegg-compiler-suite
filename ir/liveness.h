/* liveness.h : shared control-flow-aware live-interval computation */
#ifndef SKJ_IR_LIVENESS_H
#define SKJ_IR_LIVENESS_H

struct ir_func;

/*
 * Fill first_def[0..fn->ntemps) and last_use[0..fn->ntemps) with
 * control-flow-correct live intervals for the linear-scan allocator.
 *
 * first_def[t] is the position of the earliest instruction that writes t, or
 * -1 if t is never written.  last_use[t] is the last position at which t is
 * live, which extends past its last textual mention to the end of every basic
 * block in which t is live-out.  That extension is what the old textual pass
 * missed at a loop back-edge.
 *
 * Positions are the instruction's index in fn's instruction list (0-based, one
 * per node), matching each backend's own scan so the two agree.
 *
 * The caller still performs the per-backend register classification, the
 * call-position collection, and the IR_ARG to call live-range extension on top
 * of these arrays; all of those only extend last_use further and compose with
 * this result.
 */
void ir_live_intervals(struct ir_func *fn, int *first_def, int *last_use);

#endif
