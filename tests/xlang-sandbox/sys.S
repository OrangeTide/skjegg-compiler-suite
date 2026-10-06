/* sys.S : the one emulator-private ecall stub the cross-language sandbox parent
   needs. The RISC-V psABI already delivers the C arguments in a0, a1; the stub
   sets a7 to the spawn call number and traps, and the handle comes back in a0.
   skj-cc-rv has no operand-constrained inline asm, so it lives here and is
   assembled by skj-as-rv. (write / exit come from the skj crt, start_rv.S.) */

    .text

    /* unsigned sys_spawn(const void *img, unsigned len);  a0=img a1=len -> a0=handle */
    .globl sys_spawn
sys_spawn:
    li  a7, 0x000f0003
    ecall
    ret
