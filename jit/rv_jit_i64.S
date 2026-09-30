/* rv_jit_i64.S : the RV32 stack-ABI 64-bit helpers, for the in-process RV JIT.

   RV32 has no native 64-bit register, so the backend lowers a 64-bit multiply
   and the shift family to calls into these helpers, passing the operands on the
   stack (not in registers) and taking the result in a0:a1.  That is an internal
   ABI between the backend and its runtime, distinct from the C register ABI.

   The AOT runtime supplies these from runtime/start_rv.S.  The JIT driver is an
   ordinary C program whose own libgcc __muldi3 (if any) uses the C register ABI,
   so these carry distinct names (skj_jit_*) and the driver binds the guest's
   __muldi3 / __ashldi3 / __ashrdi3 / __lshrdi3 to them.  The bodies are copied
   verbatim from runtime/start_rv.S; they are leaf code that touches only a0-a3
   and t0-t2 and reads its operands relative to the caller's sp. */

    .text

/* skj_jit_muldi3: Stack 8(sp)=a_lo 12(sp)=a_hi 0(sp)=b_lo 4(sp)=b_hi -> a0:a1 */
    .globl skj_jit_muldi3
skj_jit_muldi3:
    lw   a0, 8(sp)
    lw   a1, 12(sp)
    lw   a2, 0(sp)
    lw   a3, 4(sp)
    mulhu t0, a0, a2
    mul  t1, a1, a2
    add  t0, t0, t1
    mul  t1, a0, a3
    add  a1, t0, t1
    mul  a0, a0, a2
    ret

/* skj_jit_ashldi3: Stack 0(sp)=shift 4(sp)=a_lo 8(sp)=a_hi -> a0:a1 */
    .globl skj_jit_ashldi3
skj_jit_ashldi3:
    lw   a2, 0(sp)
    lw   a0, 4(sp)
    lw   a1, 8(sp)
    li   t0, 32
    bge  a2, t0, .Lashl_ge32
    beqz a2, .Lashl_done
    sll  a1, a1, a2
    sub  t1, t0, a2
    srl  t2, a0, t1
    or   a1, a1, t2
    sll  a0, a0, a2
    ret
.Lashl_ge32:
    li   t0, 64
    bge  a2, t0, .Lashl_zero
    addi a2, a2, -32
    sll  a1, a0, a2
    li   a0, 0
    ret
.Lashl_zero:
    li   a0, 0
    li   a1, 0
.Lashl_done:
    ret

/* skj_jit_ashrdi3: Stack 0(sp)=shift 4(sp)=a_lo 8(sp)=a_hi -> a0:a1 */
    .globl skj_jit_ashrdi3
skj_jit_ashrdi3:
    lw   a2, 0(sp)
    lw   a0, 4(sp)
    lw   a1, 8(sp)
    li   t0, 32
    bge  a2, t0, .Lashr_ge32
    beqz a2, .Lashr_done
    srl  a0, a0, a2
    sub  t1, t0, a2
    sll  t2, a1, t1
    or   a0, a0, t2
    sra  a1, a1, a2
    ret
.Lashr_ge32:
    li   t0, 64
    bge  a2, t0, .Lashr_sign
    addi a2, a2, -32
    sra  a0, a1, a2
    srai a1, a1, 31
    ret
.Lashr_sign:
    srai a1, a1, 31
    mv   a0, a1
.Lashr_done:
    ret

/* skj_jit_lshrdi3: Stack 0(sp)=shift 4(sp)=a_lo 8(sp)=a_hi -> a0:a1 */
    .globl skj_jit_lshrdi3
skj_jit_lshrdi3:
    lw   a2, 0(sp)
    lw   a0, 4(sp)
    lw   a1, 8(sp)
    li   t0, 32
    bge  a2, t0, .Llshr_ge32
    beqz a2, .Llshr_done
    srl  a0, a0, a2
    sub  t1, t0, a2
    sll  t2, a1, t1
    or   a0, a0, t2
    srl  a1, a1, a2
    ret
.Llshr_ge32:
    li   t0, 64
    bge  a2, t0, .Llshr_zero
    addi a2, a2, -32
    srl  a0, a1, a2
    li   a1, 0
    ret
.Llshr_zero:
    li   a0, 0
    li   a1, 0
.Llshr_done:
    ret

    .section .note.GNU-stack,"",%progbits
