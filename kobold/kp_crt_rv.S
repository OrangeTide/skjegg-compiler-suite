/* kp_crt_rv.S : the freestanding RV32 start file for a Kobold guest program.

   The counterpart of the hosted aot_rt.c entry, for a host whose seam to the
   guest is only write/read/brk/_exit.  It runs on the stack the host hands in
   (the entry sp), records its argument vector, and enters the program through
   kp_guest_entry, the name skj-kp-rv gives the program's entry.  A normal return
   exits 0 (the hosted crt does the same); a program ends nonzero only by calling
   Halt(code), which exits through io_halt.

   The guest heap grows the program break on demand (rt_free.c over the brk
   syscall), so this file reserves no heap region and no longer calls kp_rt_init;
   the host sets the break's base and limit when it loads the task (boris A3).

   The entry argument vector is whatever the host places at the entry sp: argc
   then the argv pointers then a null, the ordinary ELF convention qemu-riscv32
   follows.  A host that passes none can leave the count word zero. */

    .text
    .globl _start
_start:
    mv      a0, sp                      /* a0 = the raw entry vector (argc, argv..) */
    call    kp_set_raw_args             /* record it for ParamCount / ParamStr */

    call    kp_guest_entry              /* run the program */

    /* A normal return exits 0, as the hosted crt does; a program ends with a
       nonzero status only by calling Halt(code), which exits through io_halt. */
    li      a0, 0
    call    io_halt                     /* flush buffered stdio, then _exit(0) */
1:  j       1b

    /* Entry-ABI version guard (kp_abi.h): reference the archive's version symbol
       so a crt built for one ABI version fails to link against a libkpaot-*-free
       of another, rather than silently mismatching the entry seam.  A downstream
       host that writes its own crt should carry the same reference.  Bump the
       suffix here with KP_GUEST_ABI_VERSION. */
    .section .rodata
    .align  2
    .globl  __kp_crt_abi_ref
__kp_crt_abi_ref:
    .word   kp_guest_abi_v3
