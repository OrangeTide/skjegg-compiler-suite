// hello_arm64.s : minimal runnable program for the skj-ld-arm64 link test.
// Exercises a PC-relative call (R_AARCH64_CALL26), an ADRP+ADD address pair
// (R_AARCH64_ADR_PREL_PG_HI21 + R_AARCH64_ADD_ABS_LO12_NC) and an absolute
// 64-bit data pointer (R_AARCH64_ABS64 via .quad).  Writes "ok\n", exits 42.
.text
.globl _start
_start:
    bl do_write                // R_AARCH64_CALL26
    mov x8, #93                // exit
    mov x0, #42
    svc #0
do_write:
    adrp x0, ptrq              // R_AARCH64_ADR_PREL_PG_HI21
    add x0, x0, ptrq           // R_AARCH64_ADD_ABS_LO12_NC
    ldr x1, [x0]               // &msg from the R_AARCH64_ABS64 slot
    mov x8, #64                // write
    mov x0, #1
    mov x2, #3
    svc #0
    ret
.section .rodata
msg:
    .ascii "ok\n"
.data
ptrq:
    .quad msg                  // R_AARCH64_ABS64
