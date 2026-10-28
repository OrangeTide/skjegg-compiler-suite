# mov: rr, imm32, imm64 (movabs), imm32-fits (C7), offset sym, loads/stores
.intel_syntax noprefix
.text
.globl movs
movs:
    mov rax, rbx
    mov rcx, rdx
    mov r8, r15
    mov eax, ebx
    mov r9d, ecx
    mov ax, bx
    mov al, bl
    mov eax, 5
    mov rax, 5
    mov rax, -1
    mov r11, 0x8000000000000000
    mov rbx, 0x1122334455667788
    mov rax, offset gsym
    mov eax, offset gsym
    mov rax, qword ptr [rbp-8]
    mov dword ptr [rbp-4], ecx
    mov qword ptr [rsp+16], rax
    mov byte ptr [rdi], al
    mov word ptr [rsi], bx
    mov qword ptr [rbp-24], offset gsym
    mov dword ptr [rbp-28], 7
    ret
.data
.globl gsym
gsym:
    .quad 0
