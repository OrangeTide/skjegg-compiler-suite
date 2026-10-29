/* vmm_selftest_guest.S : a minimal HV guest for the VMM self-test.
 * Makes a write and an exit syscall through the doorbell, then halts.
 * A flat binary loaded at guest-physical 0x100000. */

    .code64
    .text
    .globl _start
_start:
    /* write(1, msg, msglen) */
    mov $1, %rax                /* nr = write */
    mov $1, %rdi                /* fd = 1 */
    lea msg(%rip), %rsi         /* buf */
    mov $(msg_end - msg), %rdx  /* len */
    movq 0x30000000, %rax       /* doorbell: result -> rax */

    /* exit(42) */
    mov $60, %rax               /* nr = exit */
    mov $42, %rdi               /* code */
    movq 0x30000000, %rax       /* doorbell (does not return) */

    hlt

msg:
    .ascii "hello vmm\n"
msg_end:
