/* core_portme.c : CoreMark port for the RV32 emulator */
/* Copyright (c) 2026 Jon Mayo - MIT-0 OR Public Domain */

/* CoreMark is a real third-party benchmark rather than something written
 * alongside the emulator, which is the point of running it: it exercises
 * list manipulation, matrix arithmetic and a state machine over code the
 * emulator's author had no hand in.
 *
 * The barebones port CoreMark ships expects a board to supply four things:
 * a clock, board initialization, a character sink, and the seed variables.
 * Here the clock is the cycle counter the emulator maintains in a control
 * register, and the character sink is one ecall per character. There is no
 * board to initialize. */

#include "coremark.h"

#if VALIDATION_RUN
volatile ee_s32 seed1_volatile = 0x3415;
volatile ee_s32 seed2_volatile = 0x3415;
volatile ee_s32 seed3_volatile = 0x66;
#endif
#if PERFORMANCE_RUN
volatile ee_s32 seed1_volatile = 0x0;
volatile ee_s32 seed2_volatile = 0x0;
volatile ee_s32 seed3_volatile = 0x66;
#endif
#if PROFILE_RUN
volatile ee_s32 seed1_volatile = 0x8;
volatile ee_s32 seed2_volatile = 0x8;
volatile ee_s32 seed3_volatile = 0x8;
#endif
volatile ee_s32 seed4_volatile = ITERATIONS;
volatile ee_s32 seed5_volatile = 0;

ee_u32 default_num_contexts = 1;

/****************************************************************
 * Console
 *
 * The write syscall the emulator's machine layer already provides. One
 * call per character is wasteful, but CoreMark only prints a report at
 * the end and the timed section produces no output at all.
 ****************************************************************/

#define SYS_WRITE 64

void
uart_send_char(char c)
{
    register ee_u32 a0 __asm__("a0") = 1;
    register char *a1 __asm__("a1") = &c;
    register ee_u32 a2 __asm__("a2") = 1;
    register ee_u32 a7 __asm__("a7") = SYS_WRITE;

    __asm__ volatile("ecall"
                     : "+r"(a0)
                     : "r"(a1), "r"(a2), "r"(a7)
                     : "memory");
}

/****************************************************************
 * Timing
 *
 * rdcycle reads the retired-instruction counter the emulator keeps. On
 * real hardware this counts clock cycles; here one instruction is one
 * tick, so the reported iterations per second is a count of emulated
 * instructions and not a wall-clock rate. The score is therefore not a
 * comparable CoreMark result, and the report says so.
 ****************************************************************/

static CORE_TICKS start_ticks, stop_ticks;

static ee_u32
read_cycles(void)
{
    ee_u32 v;

    __asm__ volatile("csrr %0, cycle" : "=r"(v));
    return v;
}

void
start_time(void)
{
    start_ticks = read_cycles();
}

void
stop_time(void)
{
    stop_ticks = read_cycles();
}

CORE_TICKS
get_time(void)
{
    return stop_ticks - start_ticks;
}

secs_ret
time_in_secs(CORE_TICKS ticks)
{
    return (secs_ret)(ticks / EE_TICKS_PER_SEC);
}

/****************************************************************
 * Board
 ****************************************************************/

void
portable_init(core_portable *p, int *argc, char *argv[])
{
    (void)argc;
    (void)argv;
    p->portable_id = 1;
}

void
portable_fini(core_portable *p)
{
    p->portable_id = 0;
}
