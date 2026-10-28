# SSE: movsd/movss, arith, compares, conversions, logical, movq/movd
.intel_syntax noprefix
.text
.globl sse
sse:
    movsd xmm0, xmm1
    movsd xmm2, qword ptr [rbp-16]
    movsd qword ptr [rbp-16], xmm3
    movss xmm0, xmm1
    movss xmm4, dword ptr [rbp-4]
    movss dword ptr [rbp-4], xmm5
    addsd xmm0, xmm1
    subsd xmm2, xmm3
    mulsd xmm4, xmm5
    divsd xmm6, xmm7
    sqrtsd xmm0, xmm1
    ucomisd xmm0, xmm1
    addss xmm8, xmm9
    ucomiss xmm0, xmm1
    cvtss2sd xmm0, xmm1
    cvtsd2ss xmm2, xmm3
    cvtsi2sd xmm0, eax
    cvtsi2ss xmm1, ecx
    cvttsd2si eax, xmm3
    cvttss2si ecx, xmm4
    xorpd xmm1, xmm1
    xorps xmm2, xmm2
    andpd xmm1, xmm2
    movq rax, xmm0
    movq xmm0, rax
    movd eax, xmm1
    movd xmm1, eax
    ret
