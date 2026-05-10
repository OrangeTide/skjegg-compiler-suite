# Open Questions

1. ~~What minimal IR support do we need for 64-bit integers on 32-bit
   backends?~~
   **Resolved:** Target-specific lowering via register-pair allocation.
   Each backend emits optimal sequences (ColdFire addx/subx, RISC-V
   sltu+add). Complex ops (mul, shifts) call libgcc helpers. 27 new IR
   opcodes, three-class linear-scan regalloc (I64/I32/float).

2. RISC-V continuations and float codegen need to be implemented, not
   just stubbed. PicoCalc (Raspberry Pi Pico 2, Hazard3 RV32IMAC) is a
   concrete target for compiled TinScheme. The capture/resume save area
   must be written from scratch for RV32's s1-s11 callee-save
   convention. Float codegen depends on the F extension availability
   (Hazard3 does not have hardware float; Cortex-M33F does via its ARM
   mode).

3. ~~WASM backend structured control flow.~~
   **Deferred:** WASM backend is low priority. When revisited, a
   relooper is required because the C compiler's `goto` creates
   irreducible control flow in real-world code (backward jumps are
   common).
