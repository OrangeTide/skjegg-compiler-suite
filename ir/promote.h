/* promote.h : scalar register promotion, a shared IR-to-IR pass.

   A local slot that is a plain word scalar, is never address-taken, and is
   touched only by IR_LDL / IR_STL never escapes to memory, so its value can live
   in a register across its whole (cross-block) live range instead of
   round-tripping through the frame.  The pass turns each such slot into one temp:
   every store becomes a move into it, every load a move out of it.  The
   control-flow-aware liveness pass then gives the temp a cross-block interval and
   the allocator keeps it in a register, or spills it back to a slot under
   pressure (reproducing the original memory traffic, never worse).

   It is backend-neutral and runs before register allocation.  It is on by
   default; SKJ_NO_PROMOTE disables it as an escape hatch for measurement and
   for bisecting a suspected promotion bug. */

#ifndef IR_PROMOTE_H
#define IR_PROMOTE_H

struct ir_func;

/* promote_wide selects whether 8-byte slots are promoted.  On the LP64 targets
   such a slot is a pointer or long in a single register and promotion is a plain
   win.  On the ILP32 targets it is an i64 held in a register pair, and pinning a
   pair across a whole live range can cost more in evicted spills than the frame
   round-trips it removes.  Measured on the C corpus that trade is a net win on
   RISC-V and ColdFire but a net loss on MIPS, the most register-poor target, so
   the MIPS backend passes 0 here and every other backend passes 1. */
void ir_promote_slots(struct ir_func *fn, int promote_wide);

#endif /* IR_PROMOTE_H */
