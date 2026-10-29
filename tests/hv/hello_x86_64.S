/* hello_x86_64.S : a minimal x86-64 HV guest for the skj-run end-to-end test.
 * Runs natively under KVM, reaches the sandbox through the syscall doorbell, and
 * writes a line then exits 42.  GNU .intel_syntax, assembled by skj-as-x86-64. */

    .intel_syntax noprefix
    .text
    .globl _start
_start:
    mov rax, 1                  /* write */
    mov rdi, 1                  /* fd = 1 */
    lea rsi, [rip + msg]        /* buf */
    mov rdx, msg_end - msg      /* len */
    mov rax, [0x30000000]       /* doorbell: result -> rax */

    mov rax, 60                 /* exit */
    mov rdi, 42                 /* code */
    mov rax, [0x30000000]       /* doorbell (does not return) */

spin:
    jmp spin                    /* never reached; guards against a stray return */

msg:
    .ascii "hello hv x86\n"
msg_end:
