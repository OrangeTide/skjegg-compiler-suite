# data directives: byte/word/long/quad, symbol refs, strings, zero, align
.intel_syntax noprefix
.data
.globl d
d:
    .byte 1, 2, 3, 0xff
    .word 0x1234, 42
    .long 0x12345678, -1
    .quad 0x1122334455667788
    .quad dsym
    .long dsym
    .quad dsym + 16
    .align 16
    .zero 8
.globl dsym
dsym:
    .quad 0
.section .rodata
.globl rostr
rostr:
    .string "hello"
    .ascii "abc"
