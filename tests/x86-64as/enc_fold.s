# expression folding: constant arithmetic and label-difference literals, all
# local so the comparison with GNU as is pure bytes (no relocations).
.intel_syntax noprefix
.text
.globl f
f:
    mov rax, 2 + 3 * 4          # 0xe
    mov rcx, (10 - 2) * 2       # 0x10
    mov rdx, -5 + 8             # 0x3
    mov rbx, 1 * 2 * 3 * 4      # 0x18
    mov edi, end - start        # a forward label difference
    mov rsi, end - start
    add rbx, end - start
    sub rcx, end - start
start:
    .byte 0x90, 0x90, 0x90
end:
    nop

.data
.globl d
d:
    .byte  end - start          # 3
    .word  end - start
    .long  end - start
    .quad  end - start
    .long  (end - start) * 4 + 1
    .quad  2 + 2
