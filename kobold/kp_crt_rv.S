/* kp_crt_rv.S : the freestanding RV32 start file for a Kobold guest program.

   The counterpart of the hosted aot_rt.c entry, for a host whose seam to the
   guest is only write/read/_exit.  It runs on the stack the host hands in (the
   entry sp), hands the runtime its argument vector and its guest heap region, and
   enters the program through kp_guest_entry, the name skj-kp-rv gives the
   program's entry.  A normal return exits 0 (the hosted crt does the same); a
   program ends nonzero only by calling Halt(code), which exits through io_halt.

   The guest heap is handed in at entry: this file reserves a bss region and
   passes its base and size to kp_rt_init.  A downstream host that owns its memory
   supplies its own region instead (call kp_rt_init with it and jump to
   kp_guest_entry); nothing here depends on a syscall but the exit the runtime
   makes through io_free.c.

   The entry argument vector is whatever the host places at the entry sp: argc
   then the argv pointers then a null, the ordinary ELF convention qemu-riscv32
   follows.  A host that passes none can leave the count word zero. */

/* KP_HEAP_SIZE is 8 MiB (8388608); skj-as-rv has no .equ, so the literal is
   repeated in the li and the .space below.  Keep the two in step. */

    .text
    .globl _start
_start:
    mv      a0, sp                      /* a0 = the raw entry vector (argc, argv..) */
    call    kp_set_raw_args             /* record it for ParamCount / ParamStr */

    la      a0, __kp_heap               /* hand the runtime its guest heap region */
    li      a1, 8388608                 /* KP_HEAP_SIZE */
    call    kp_rt_init

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

    .bss
    .align  4                           /* 16-aligned base, as the allocator expects */
    .globl  __kp_heap
__kp_heap:
    .space  8388608                     /* KP_HEAP_SIZE */
