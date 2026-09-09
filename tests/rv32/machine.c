/* machine.c : flat memory and syscall environment for the RV32 emulator */
/* Copyright (c) 2026 Jon Mayo - MIT-0 OR Public Domain */

#include "machine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/****************************************************************
 * Memory bus
 *
 * The guest never sees a host pointer. Every access goes through these
 * callbacks, so a host embedding can substitute memory-mapped devices or
 * a tighter sandbox without touching the interpreter.
 ****************************************************************/

static uint32_t
mem_r8(void *ctx, uint32_t addr)
{
    machine *m = ctx;

    if (addr >= m->mem_size)
        return 0;
    return m->mem[addr];
}

static uint32_t
mem_r16(void *ctx, uint32_t addr)
{
    machine *m = ctx;

    if (addr + 1 >= m->mem_size)
        return 0;
    return (uint32_t)m->mem[addr] | ((uint32_t)m->mem[addr + 1] << 8);
}

static uint32_t
mem_r32(void *ctx, uint32_t addr)
{
    machine *m = ctx;

    if (addr + 3 >= m->mem_size)
        return 0;
    return (uint32_t)m->mem[addr] | ((uint32_t)m->mem[addr + 1] << 8) |
           ((uint32_t)m->mem[addr + 2] << 16) |
           ((uint32_t)m->mem[addr + 3] << 24);
}

static void
mem_w8(void *ctx, uint32_t addr, uint32_t val)
{
    machine *m = ctx;

    if (addr < m->mem_size)
        m->mem[addr] = (uint8_t)val;
}

static void
mem_w16(void *ctx, uint32_t addr, uint32_t val)
{
    machine *m = ctx;

    if (addr + 1 < m->mem_size) {
        m->mem[addr] = (uint8_t)val;
        m->mem[addr + 1] = (uint8_t)(val >> 8);
    }
}

static void
mem_w32(void *ctx, uint32_t addr, uint32_t val)
{
    machine *m = ctx;

    if (addr + 3 < m->mem_size) {
        m->mem[addr] = (uint8_t)val;
        m->mem[addr + 1] = (uint8_t)(val >> 8);
        m->mem[addr + 2] = (uint8_t)(val >> 16);
        m->mem[addr + 3] = (uint8_t)(val >> 24);
    }
}

/****************************************************************
 * Syscall layer
 *
 * Only the handful of Linux calls that a freestanding test program can
 * reach. Matching qemu-riscv32's user-mode behaviour for these is what
 * lets the same binary run under both models and be compared instruction
 * by instruction.
 ****************************************************************/

#define SYS_WRITE       64
#define SYS_EXIT        93
#define SYS_EXIT_GROUP  94

static int
do_ecall(rv_cpu *cpu, void *ctx)
{
    machine *m = ctx;
    uint32_t nr = cpu->x[17];       /* a7 */
    uint32_t a0 = cpu->x[10];
    uint32_t a1 = cpu->x[11];
    uint32_t a2 = cpu->x[12];
    uint32_t i;

    m->syscalls++;

    switch (nr) {
    case SYS_WRITE:
        if (!m->quiet && (a0 == 1 || a0 == 2)) {
            for (i = 0; i < a2 && a1 + i < m->mem_size; i++)
                fputc(m->mem[a1 + i], a0 == 1 ? stdout : stderr);
            fflush(a0 == 1 ? stdout : stderr);
        }
        cpu->x[10] = a2;
        return 0;

    case SYS_EXIT:
    case SYS_EXIT_GROUP:
        m->exited = 1;
        m->exit_code = (int)a0;
        rv_halt(cpu);
        return 0;

    default:
        /* Unknown call: report failure the way Linux would */
        cpu->x[10] = (uint32_t)-38;     /* -ENOSYS */
        return 0;
    }
}

/****************************************************************
 * Public interface
 ****************************************************************/

void
machine_poke(void *ctx, uint32_t addr, uint8_t byte)
{
    machine *m = ctx;

    if (addr < m->mem_size)
        m->mem[addr] = byte;
}

int
machine_init(machine *m, rv_cpu *cpu)
{
    memset(m, 0, sizeof(*m));
    m->mem_size = MACHINE_MEM_SIZE;
    m->mem = calloc(1, m->mem_size);
    if (!m->mem) {
        fprintf(stderr, "machine: out of memory\n");
        return -1;
    }

    rv_init(cpu, mem_r8, mem_r16, mem_r32, mem_w8, mem_w16, mem_w32, m);
    rv_set_ecall(cpu, do_ecall, m);
    return 0;
}

void
machine_free(machine *m)
{
    free(m->mem);
    m->mem = NULL;
}
