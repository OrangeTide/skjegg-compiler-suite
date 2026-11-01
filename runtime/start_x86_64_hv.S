/* start_x86_64_hv.S : the ELF entry for the hardware-virtualization guest.
   The scheduler sets rsp to the LP64 initial stack (argc, argv, envp, auxv) and
   the pc to _start.  Read argc/argv, align the stack, call main, and exit with
   its return value through the syscall seam. */

    .intel_syntax noprefix
    .text
    .globl _start
    .weak environ               /* set below only when the libc links it */
_start:
    mov rdi, [rsp]              /* argc */
    lea rsi, [rsp + 8]          /* argv */
    lea rdx, [rsi + rdi*8 + 8]  /* envp = argv + (argc + 1) slots */
    lea rax, [rip + environ]    /* &environ, 0 when the libc omits it (weak) */
    test rax, rax
    jz 1f
    mov [rax], rdx
1:
    and rsp, -16                /* System V: 16-byte aligned before the call */
    call main
    mov rdi, rax                /* exit code (low 32 bits of the return) */
    call exit
.Lhalt:
    jmp .Lhalt
