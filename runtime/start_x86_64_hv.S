/* start_x86_64_hv.S : the ELF entry for the hardware-virtualization guest.
   The scheduler sets rsp to the LP64 initial stack (argc, argv, envp, auxv) and
   the pc to _start.  Read argc/argv, align the stack, call main, and exit with
   its return value through the syscall seam. */

    .intel_syntax noprefix
    .text
    .globl _start
_start:
    mov rdi, [rsp]              /* argc */
    lea rsi, [rsp + 8]          /* argv */
    and rsp, -16                /* System V: 16-byte aligned before the call */
    call main
    mov rdi, rax                /* exit code (low 32 bits of the return) */
    call exit
.Lhalt:
    jmp .Lhalt
