| crt_entry_cf.S : the shared m68k/ColdFire _start, included by both the C
| runtime (runtime/start.S, assembled by GNU as) and the Kobold freestanding
| crt (kobold/kp_crt_cf.S, assembled by skj-as).  One entry, one argv contract.
|
| It reads the entry argument vector the host places at the entry sp (argc at
| (sp), the argv pointers just above, then a NULL), switches to the larger
| stack the including file reserves as stack_top, passes argc/argv to main, runs
| the weak __libc_fini finalizer when one is linked, then exits with main's
| return.
|
| The including file provides stack_top (its own bss stack) and main.  A C
| program's main is the program; a Kobold program's main is the entry shim in
| kobold/io_free.c, which records the args for ParamStr/ParamCount and runs
| kp_guest_entry.  environ and __libc_fini are weak: set or called only when a
| libc links them (the Kobold runtime defines both).  Both languages read argc
| and argv from the same bytes, so ParamStr and C argv are identical.

    .globl _start
    .weak  __libc_fini          | provided by libc; absent (0) when freestanding
    .weak  environ              | set below only when the libc links it

_start:
    move.l  %sp, %a1            | entry stack: argc at (%a1), argv just above
    lea     stack_top, %sp      | switch to our own stack (qemu-m68k's
    sub.l   #16, %sp            | default stack is a single page; recursive
                                | tests overflow it within a few frames)
    move.l  (%a1), %d0          | argc
    lea     4(%a1), %a0         | argv (the pointers stay mapped in the entry
                                | stack region)
    move.l  %d0, %d1            | envp = argv + (argc+1) words: skip the argv[]
    addq.l  #1, %d1             | entries and their NULL terminator
    lsl.l   #2, %d1
    move.l  %a0, %a2
    add.l   %d1, %a2            | a2 = envp
    lea     environ, %a3        | weak: present only when the libc links it
    move.l  %a3, %d1
    tst.l   %d1
    beq     3f
    move.l  %a2, (%a3)
3:
    move.l  %a0, -(%sp)         | push main's args right-to-left (argv, argc),
    move.l  %d0, -(%sp)         | the toolkit/m68k C ABI; caller pops
    jsr     main
    addq.l  #8, %sp             | pop the two args
    move.l  %d0, %d2            | preserve main's return across the finalizer
    lea     __libc_fini, %a0    | weak: 0 when no libc is linked
    move.l  %a0, %d0
    tst.l   %d0
    beq     1f
    jsr     (%a0)               | run atexit handlers and flush stdio
1:
    move.l  %d2, %d1            | exit status
    move.l  #1, %d0             | __NR_exit
    trap    #0
    | unreachable
2:  bra     2b
