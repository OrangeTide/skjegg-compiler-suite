# branches to external symbols: call/jmp/jCC all rel32 (PLT32)
.intel_syntax noprefix
.text
.globl branches
branches:
    call ext_a
    jmp ext_b
    je ext_c
    jne ext_c
    jl ext_c
    jge ext_c
    jle ext_c
    jg ext_c
    jb ext_c
    jae ext_c
    ja ext_c
    jbe ext_c
    js ext_c
    jns ext_c
    call rax
    call qword ptr [rbp-8]
    jmp r11
    ret
