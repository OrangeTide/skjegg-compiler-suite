# memory addressing: base, base+disp8/disp32, base+index*scale, rbp/r13/rsp/r12
.intel_syntax noprefix
.text
.globl mem
mem:
    mov rax, [rbx]
    mov rax, [rbp]
    mov rax, [rsp]
    mov rax, [r12]
    mov rax, [r13]
    mov rax, [rbx+8]
    mov rax, [rbx-8]
    mov rax, [rbx+1000]
    mov rax, [rbx+rcx]
    mov rax, [rbx+rcx*4+16]
    mov rax, [rbx+rcx*8]
    mov rax, [r8+r9*2+4]
    mov rax, [rsp+rax*4+32]
    lea rax, [rbx+rcx*4+16]
    lea rdx, [rip+gsym2]
    mov rax, [rip+gsym2]
    ret
.data
.globl gsym2
gsym2:
    .quad 0
