/* setjmp_x86_64.S : x86-64 SysV setjmp/longjmp.  jmp_buf (a long[32]) holds the
   callee-saved registers longjmp must restore (rbx, rbp, r12-r15), the caller's
   stack pointer, and the return address.  Float/SSE is not saved: the compiler
   this libc serves keeps no live xmm callee state across a setjmp.  setjmp
   returns 0 on the direct call; longjmp resumes it returning val, or 1 if val
   is 0. */

    .intel_syntax noprefix
    .text
    .globl setjmp
    .globl longjmp

setjmp:
    mov [rdi + 0], rbx
    mov [rdi + 8], rbp
    mov [rdi + 16], r12
    mov [rdi + 24], r13
    mov [rdi + 32], r14
    mov [rdi + 40], r15
    lea rax, [rsp + 8]          /* caller's rsp (past this call's return addr) */
    mov [rdi + 48], rax
    mov rax, [rsp]              /* return address */
    mov [rdi + 56], rax
    xor eax, eax
    ret

longjmp:
    mov rbx, [rdi + 0]
    mov rbp, [rdi + 8]
    mov r12, [rdi + 16]
    mov r13, [rdi + 24]
    mov r14, [rdi + 32]
    mov r15, [rdi + 40]
    mov rsp, [rdi + 48]
    mov rdx, [rdi + 56]         /* return address */
    mov eax, esi               /* val ... */
    test eax, eax
    jnz 1f
    mov eax, 1                 /* ... but never 0 */
1:
    jmp rdx
