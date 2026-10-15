/* io_free_rv.S : RV32 host-seam syscall wrappers for the freestanding guest.
 *
 * The seam io_free.c builds on (kp_sys_write / read / exit) is one ecall each:
 * a7 is the syscall number, a0..a2 the arguments, the result comes back in a0.
 * The functions follow the RISC-V ILP32 psABI, so the C arguments already arrive
 * in a0..a2 and only a7 has to be set; a caller built by skj-cc-rv or by rv gcc
 * reaches them the same way.  Expressing the ecall in C needs register-bound
 * extended inline asm, which a compiler without it cannot build, so the wrappers
 * live here and io_free.c is plain C.  There is no open / close here: the rv32
 * freestanding console has only write / read / exit (KP_HAVE_FILE_IO is ColdFire
 * only). */

    .text

    .globl kp_sys_write
kp_sys_write:
    li      a7, 64              /* SYS_write; a0=fd a1=buf a2=len already in place */
    ecall
    ret

    .globl kp_sys_read
kp_sys_read:
    li      a7, 63              /* SYS_read */
    ecall
    ret

    .globl kp_sys_exit
kp_sys_exit:
    li      a7, 93              /* SYS_exit; a0=code */
    ecall
1:  j       1b                  /* never returns */
