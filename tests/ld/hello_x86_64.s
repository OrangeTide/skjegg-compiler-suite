# hello_x86_64.s : minimal runnable program for the skj-ld-x86-64 link test.
# Exercises a PC-relative call (R_X86_64_PLT32), an absolute 32-bit symbol
# reference (R_X86_64_32 via `offset`), and an absolute 64-bit data pointer
# (R_X86_64_64 via `.quad`).  Writes "ok\n" and exits 42.
.intel_syntax noprefix
.text
.globl _start
_start:
    call do_write              # R_X86_64_PLT32
    mov rax, 60                # exit
    mov rdi, 42
    syscall
do_write:
    mov rsi, qword ptr [ptrq]  # &msg from the R_X86_64_64 slot
    mov eax, offset msg        # R_X86_64_32 (unused result; forces the reloc)
    mov rax, 1                 # write
    mov rdi, 1
    mov rdx, 3
    syscall
    ret
.section .rodata
msg:
    .ascii "ok\n"
.data
ptrq:
    .quad msg                  # R_X86_64_64
