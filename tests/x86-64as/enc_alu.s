# add/sub/and/or/xor/cmp: rr, imm8, imm32, mem forms; test; neg/not
.intel_syntax noprefix
.text
.globl alu
alu:
    add rax, rbx
    add eax, ecx
    sub rsp, 32
    sub rsp, 0x128
    and eax, -16
    and rax, 7
    or rbx, rax
    xor r8, r9
    xor eax, eax
    cmp eax, 0
    cmp rax, 100000
    add rax, qword ptr [rbp-8]
    sub dword ptr [rbp-4], ecx
    cmp qword ptr [rsp], rdx
    test rax, rax
    test eax, ecx
    neg rax
    not rbx
    neg eax
    ret
