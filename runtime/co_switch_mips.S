/* co_switch_mips.S : MIPS I (o32) context switch for the coroutine runtime */

/* void co_switch(struct co_ctx *from, struct co_ctx *to)
 *   o32 psABI: from = $a0, to = $a1; co_ctx.sp is at offset 0.
 * Save the callee-saved registers on the current stack, stash sp into *from,
 * load sp from *to, restore its callee-saved registers, and jr $ra into
 * whatever that stack was about to return to. The saved $ra rides the switched
 * stack, so a plain jal/jr carries control across the switch. The callee-saved
 * set is o32's: $s0-$s7, $fp ($s8), $ra, and the callee-saved FPU registers
 * $f20-$f31, saved as single words so no sdc1 is needed (FR=0 double pairs are
 * contiguous). A suspended coroutine may hold live floats, so they are saved.
 * .set noreorder: the delay slot after jr $ra is filled explicitly. The 96-byte
 * block has $ra at offset 36, which the seed in coro.c fills with the
 * trampoline for a fresh coroutine. */

    .text
    .set noreorder
    .globl co_switch
co_switch:
    addiu $sp, $sp, -96
    sw    $s0, 0($sp)
    sw    $s1, 4($sp)
    sw    $s2, 8($sp)
    sw    $s3, 12($sp)
    sw    $s4, 16($sp)
    sw    $s5, 20($sp)
    sw    $s6, 24($sp)
    sw    $s7, 28($sp)
    sw    $fp, 32($sp)
    sw    $ra, 36($sp)
    swc1  $f20, 40($sp)
    swc1  $f21, 44($sp)
    swc1  $f22, 48($sp)
    swc1  $f23, 52($sp)
    swc1  $f24, 56($sp)
    swc1  $f25, 60($sp)
    swc1  $f26, 64($sp)
    swc1  $f27, 68($sp)
    swc1  $f28, 72($sp)
    swc1  $f29, 76($sp)
    swc1  $f30, 80($sp)
    swc1  $f31, 84($sp)

    sw    $sp, 0($a0)          # from->sp = sp
    lw    $sp, 0($a1)          # sp = to->sp

    lw    $s0, 0($sp)
    lw    $s1, 4($sp)
    lw    $s2, 8($sp)
    lw    $s3, 12($sp)
    lw    $s4, 16($sp)
    lw    $s5, 20($sp)
    lw    $s6, 24($sp)
    lw    $s7, 28($sp)
    lw    $fp, 32($sp)
    lw    $ra, 36($sp)
    lwc1  $f20, 40($sp)
    lwc1  $f21, 44($sp)
    lwc1  $f22, 48($sp)
    lwc1  $f23, 52($sp)
    lwc1  $f24, 56($sp)
    lwc1  $f25, 60($sp)
    lwc1  $f26, 64($sp)
    lwc1  $f27, 68($sp)
    lwc1  $f28, 72($sp)
    lwc1  $f29, 76($sp)
    lwc1  $f30, 80($sp)
    lwc1  $f31, 84($sp)

    jr    $ra
    addiu $sp, $sp, 96         # delay slot: pop the block
