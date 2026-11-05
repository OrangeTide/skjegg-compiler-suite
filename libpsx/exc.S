/* exc.S - libpsx exception vector and COP0 setup (milestone 1, irq.c's low
 * level).
 *
 * The VBLANK interrupt needs a first-level exception handler and the privileged
 * COP0 register moves to enable interrupts and return from one. skj-as-mips
 * encodes only what the backend emits, and the backend never emits COP0
 * instructions, so mfc0, mtc0, and rfe are written here as their raw .word
 * encodings, each named in a comment. Everything else is ordinary MIPS the
 * assembler knows. The whole file is .set noreorder: systems code owns its
 * delay slots, so every one is filled by hand.
 *
 * The handler uses only $k0 and $k1, the two registers reserved for a kernel,
 * so it saves no caller context. It acknowledges VBLANK and advances a frame
 * counter, then returns to the interrupted instruction.
 *
 * __psx_irq_install takes over the general exception vector at 0x80000080. The
 * BIOS normally owns it, so after this the BIOS interrupt services (the ones
 * the pad and CD routines use) are unavailable. Milestone 1 is GPU-only and
 * does not need them; a later milestone that does will cooperate with the
 * handler chain instead of replacing it.
 */

    .set noreorder

    .text
    .globl __psx_exc
    .globl __psx_irq_install

/* The first-level handler, installed at 0x80000080. */
__psx_exc:
    lui     $k0, 0xBF80
    lw      $k1, 0x1070($k0)        /* I_STAT */
    nop                             /* load delay slot */
    andi    $k1, $k1, 0x0001        /* VBLANK pending? */
    beq     $k1, $zero, .Lret       /* no: just return */
    nop                             /* branch delay slot */

    addiu   $k1, $zero, -2          /* 0xFFFFFFFE: clear bit 0, keep the rest */
    sw      $k1, 0x1070($k0)        /* acknowledge VBLANK ($k0 still I_STAT base) */

    la      $k0, __psx_vbl_count
    lw      $k1, 0($k0)
    nop                             /* load delay slot */
    addiu   $k1, $k1, 1
    sw      $k1, 0($k0)

.Lret:
    .word   0x401A7000              /* mfc0 $k0, $14 : EPC */
    nop                             /* mfc0 result-use delay */
    jr      $k0
    .word   0x42000010              /* rfe (branch delay slot) */

/* void __psx_irq_install(void): install the vector, flush the instruction
 * cache, and enable interrupts in the CPU. */
__psx_irq_install:
    addiu   $sp, $sp, -8
    sw      $ra, 0($sp)

    /* Build `j __psx_exc' = 0x08000000 | ((addr >> 2) & 0x03FFFFFF). */
    la      $t0, __psx_exc
    srl     $t0, $t0, 2
    li      $t1, 0x03FFFFFF
    and     $t0, $t0, $t1
    lui     $t1, 0x0800
    or      $t0, $t0, $t1

    /* Write `j __psx_exc; nop' through the uncached vector mirror 0xA0000080,
       so the store does not sit in a cache. */
    lui     $t2, 0xA000
    ori     $t2, $t2, 0x0080
    sw      $t0, 0($t2)
    sw      $zero, 4($t2)

    /* Flush the instruction cache so the fetch at 0x80000080 sees the new
       vector: BIOS A0(44h) FlushCache. */
    li      $t1, 0x44
    li      $t2, 0xA0
    jalr    $t2
    nop

    /* Enable interrupts: COP0 Status |= IM2 (0x400, the hardware interrupt line
       the interrupt controller drives) | IEc (0x1, current interrupt enable). */
    .word   0x40086000             /* mfc0 $t0, $12 : Status */
    nop                            /* mfc0 result-use delay */
    ori     $t0, $t0, 0x0401
    .word   0x40886000             /* mtc0 $t0, $12 : Status */
    nop

    lw      $ra, 0($sp)
    addiu   $sp, $sp, 8
    jr      $ra
    nop

    .data
    .align  2
    .globl  __psx_vbl_count
__psx_vbl_count:
    .word   0
