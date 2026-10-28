# shifts, imul, idiv/div, cqo/cdq, stack, rep movsq, syscall, leave/ret/nop
.intel_syntax noprefix
.text
.globl misc
misc:
    push rbp
    push r12
    pop r12
    pop rbp
    shl eax, cl
    shr rbx, cl
    sar ecx, cl
    imul eax, ecx
    imul rax, rbx
    cqo
    idiv rbx
    cdq
    idiv ecx
    div r8
    rep movsq
    syscall
    nop
    leave
    ret
.globl shifts
shifts:
    shl eax, 3
    shr rbx, 1
    sar ecx, 7
    shl r8, 1
    shr rcx, 3
    sal edx, 5
    shr r9, 0x1f
    ret
