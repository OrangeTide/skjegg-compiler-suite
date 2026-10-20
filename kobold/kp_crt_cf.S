/* kp_crt_cf.S : the freestanding ColdFire start file for a Kobold guest program.

   The ColdFire counterpart of kp_crt_rv.S, for a host whose seam to the guest is
   only write/read/exit (skj-run's CFV4e user mode, and downstream boris).  It
   records the entry argument vector for ParamCount / ParamStr, switches to its
   own execution stack, and enters the program through kp_guest_entry, the fixed
   name the AOT driver gives the program's entry.  A normal return exits 0; a
   program ends nonzero only by calling Halt(code), which exits through io_halt.

   The guest heap grows the program break on demand (rt_free.c over the brk
   syscall), so this file reserves no heap region and no longer calls kp_rt_init;
   the host sets the break's base and limit when it loads the task (boris A3).
   Only the execution stack is reserved here in bss: a deep self-compile
   overflows a small entry stack, so the crt switches to its own.

   Calls use the toolkit convention: arguments pushed right to left, the caller
   pops, results in d0/a0.  The entry argument vector is whatever the host places
   at the entry sp: argc, then the argv pointers, then a null; a host that passes
   none leaves the count word zero.

   The execution stack is 2 MiB (2097152); skj-as has no .equ, so the literal is
   repeated at its use and its .space. */

    .text
    .globl _start
_start:
    move.l  %sp, %a0            | a0 = the raw entry vector (argc, argv...)
    move.l  %a0, -(%sp)         | kp_set_raw_args(raw): record it for ParamStr
    jsr     kp_set_raw_args
    addq.l  #4, %sp

    lea     __kp_stack_top, %sp | our own stack: a deep self-compile overflows a
                                | small entry stack (start.S switches for the
                                | same reason)

    jsr     kp_guest_entry      | run the program

    move.l  #0, -(%sp)          | a normal return exits 0, as the hosted crt does;
    jsr     io_halt             | flush buffered stdio, then _exit(0)
1:  bra     1b

    /* Entry-ABI version guard (kp_abi.h): reference the archive's version symbol
       so a crt built for one ABI version fails to link against a libkpaot-*-free
       of another, rather than silently mismatching the entry seam.  Bump the
       suffix here with KP_GUEST_ABI_VERSION. */
    .data
    .align  4
    .globl  __kp_crt_abi_ref
__kp_crt_abi_ref:
    .long   kp_guest_abi_v3

    .bss
    .align  4                   | 16-aligned base
    .space  2097152             | execution stack (2 MiB)
__kp_stack_top:
