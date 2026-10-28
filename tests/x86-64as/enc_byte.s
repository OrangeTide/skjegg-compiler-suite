# byte registers (spl/bpl/sil/dil force REX), setcc, movzx/movsx, movsxd
.intel_syntax noprefix
.text
.globl byteops
byteops:
    mov sil, al
    mov dil, bl
    mov spl, cl
    mov bpl, dl
    mov byte ptr [rbp-1], sil
    setne al
    sete dl
    setl sil
    setg dil
    setbe r8b
    movzx eax, al
    movzx eax, bl
    movzx eax, dil
    movzx rax, byte ptr [rbp-1]
    movzx ecx, word ptr [rbp-2]
    movsx eax, al
    movsx rcx, word ptr [rbp-2]
    movsxd rax, ecx
    movsxd r8, dword ptr [rbp-4]
    ret
