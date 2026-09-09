/* archtest.c : runs one riscv-arch-test binary and checks its signature */
/* Copyright (c) 2026 Jon Mayo - MIT-0 OR Public Domain */

/* The compliance tests write a stream of results into a signature region
 * and then halt. A test passes when that region matches the one an
 * accepted reference model produces. The suite ships no reference
 * signatures, so this runner produces them: the target port's halt macro
 * dumps the signature as hex over the board's serial port, and the same
 * binary is run on qemu-system-riscv32 to obtain the same text.
 *
 * The dump is done by the guest rather than by reading memory through a
 * debugger, because attaching one changes what is being measured: qemu's
 * gdb stub claims the guest's own ebreak instruction instead of letting it
 * trap, which is exactly the behaviour the cebreak test exists to check.
 *
 * These binaries are bare-metal machine-mode programs at 0x80000000, not
 * the Linux user-mode images the other tools use, so the memory model and
 * the device stubs live here rather than in machine.c. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rv32.h"
#include "elf_loader.h"

#define RAM_BASE        0x80000000u
#define RAM_SIZE        (16u * 1024u * 1024u)
#define UART_BASE       0x10000000u
#define CLINT_BASE      0x02000000u
#define FINISHER_BASE   0x00100000u
#define MAX_STEPS       200000000
#define OUT_MAX         (1u << 20)

/****************************************************************
 * Memory and device model
 ****************************************************************/

typedef struct board {
    uint8_t *ram;
    rv_cpu  *cpu;
    char    *out;           /* captured serial output */
    uint32_t out_len;
    int      finished;      /* guest wrote to the test finisher */
    int      verbose;
} board;

static int
in_ram(uint32_t addr, int size)
{
    return addr >= RAM_BASE && addr + (uint32_t)size <= RAM_BASE + RAM_SIZE;
}

static uint32_t
dev_read(board *b, uint32_t addr)
{
    /* A 16550-compatible transmitter that is always ready */
    if (addr >= UART_BASE && addr < UART_BASE + 8) {
        if (addr - UART_BASE == 5)
            return 0x60;        /* holding and shift registers both empty */
        return 0;
    }
    if (addr >= CLINT_BASE && addr < CLINT_BASE + 0x10000) {
        uint32_t off = addr - CLINT_BASE;

        if (off == 0xbff8)
            return (uint32_t)b->cpu->mcycle;
        if (off == 0xbffc)
            return (uint32_t)(b->cpu->mcycle >> 32);
    }
    return 0;
}

static void
dev_write(board *b, uint32_t addr, uint32_t val)
{
    if (addr == UART_BASE) {
        if (b->out_len + 1 < OUT_MAX)
            b->out[b->out_len++] = (char)(val & 0xff);
        if (b->verbose)
            fputc((int)(val & 0xff), stderr);
        return;
    }
    if (addr == FINISHER_BASE) {
        b->finished = 1;
        rv_halt(b->cpu);
    }
}

static uint32_t
bus_r8(void *ctx, uint32_t addr)
{
    board *b = ctx;

    if (in_ram(addr, 1))
        return b->ram[addr - RAM_BASE];
    return dev_read(b, addr) & 0xff;
}

static uint32_t
bus_r16(void *ctx, uint32_t addr)
{
    board *b = ctx;

    if (in_ram(addr, 2)) {
        const uint8_t *p = b->ram + (addr - RAM_BASE);

        return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
    }
    return dev_read(b, addr) & 0xffff;
}

static uint32_t
bus_r32(void *ctx, uint32_t addr)
{
    board *b = ctx;

    if (in_ram(addr, 4)) {
        const uint8_t *p = b->ram + (addr - RAM_BASE);

        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
               ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }
    return dev_read(b, addr);
}

static void
bus_w8(void *ctx, uint32_t addr, uint32_t val)
{
    board *b = ctx;

    if (in_ram(addr, 1))
        b->ram[addr - RAM_BASE] = (uint8_t)val;
    else
        dev_write(b, addr, val);
}

static void
bus_w16(void *ctx, uint32_t addr, uint32_t val)
{
    board *b = ctx;

    if (in_ram(addr, 2)) {
        uint8_t *p = b->ram + (addr - RAM_BASE);

        p[0] = (uint8_t)val;
        p[1] = (uint8_t)(val >> 8);
    } else {
        dev_write(b, addr, val);
    }
}

static void
bus_w32(void *ctx, uint32_t addr, uint32_t val)
{
    board *b = ctx;

    if (in_ram(addr, 4)) {
        uint8_t *p = b->ram + (addr - RAM_BASE);

        p[0] = (uint8_t)val;
        p[1] = (uint8_t)(val >> 8);
        p[2] = (uint8_t)(val >> 16);
        p[3] = (uint8_t)(val >> 24);
    } else {
        dev_write(b, addr, val);
    }
}

static void
board_poke(void *ctx, uint32_t addr, uint8_t byte)
{
    board *b = ctx;

    if (in_ram(addr, 1))
        b->ram[addr - RAM_BASE] = byte;
}

/****************************************************************
 * Output handling
 ****************************************************************/

/** Keep only the lines that look like signature words, so that any
 * diagnostic text either model prints is ignored. */
static uint32_t
filter_hex(const char *in, uint32_t len, char *out)
{
    uint32_t i = 0, n = 0;

    while (i < len) {
        uint32_t start = i, j, count = 0;
        int ok = 1;

        while (i < len && in[i] != '\n')
            i++;
        for (j = start; j < i; j++) {
            char c = in[j];

            if (c == '\r')
                continue;
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
                ok = 0;
                break;
            }
            count++;
        }
        if (ok && count == 8) {
            for (j = start; j < i; j++) {
                if (in[j] != '\r')
                    out[n++] = in[j];
            }
            out[n++] = '\n';
        }
        if (i < len)
            i++;
    }
    out[n] = '\0';
    return n;
}

/** Run the same ELF on qemu-system-riscv32 and capture its serial output. */
static uint32_t
reference_output(const char *elf, char *out)
{
    char cmd[1024], *raw;
    FILE *f;
    uint32_t n = 0;

    raw = malloc(OUT_MAX);
    if (!raw)
        return 0;

    snprintf(cmd, sizeof(cmd),
             "timeout 60 qemu-system-riscv32 -machine virt -bios none "
             "-nographic -serial mon:stdio "
             "-cpu rv32,zba=true,zbb=true,zbs=true,zcb=true "
             "-kernel '%s' 2>/dev/null", elf);
    f = popen(cmd, "r");
    if (!f) {
        free(raw);
        return 0;
    }
    n = (uint32_t)fread(raw, 1, OUT_MAX - 1, f);
    pclose(f);
    raw[n] = '\0';

    n = filter_hex(raw, n, out);
    free(raw);
    return n;
}

/****************************************************************
 * Main
 ****************************************************************/

int
main(int argc, char **argv)
{
    rv_cpu cpu;
    board b;
    const char *elf;
    char *ours, *theirs;
    uint32_t entry, our_len, ref_len;
    long steps = 0;
    int i, rc = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <test.elf> [-v]\n", argv[0]);
        return 2;
    }
    elf = argv[1];
    memset(&b, 0, sizeof(b));
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0)
            b.verbose = 1;
    }

    b.ram = calloc(1, RAM_SIZE);
    b.out = malloc(OUT_MAX);
    ours = malloc(OUT_MAX);
    theirs = malloc(OUT_MAX);
    if (!b.ram || !b.out || !ours || !theirs)
        return 1;
    b.cpu = &cpu;

    rv_init(&cpu, bus_r8, bus_r16, bus_r32, bus_w8, bus_w16, bus_w32, &b);
    entry = elf_load(elf, board_poke, &b);
    if (!entry) {
        free(b.ram);
        return 1;
    }
    rv_reset(&cpu, entry);

    while (steps < MAX_STEPS && !b.finished) {
        if (rv_step(&cpu) < 0)
            break;
        steps++;
    }

    if (!b.finished) {
        uint32_t n = rv_trace_count(&cpu.trace), k;

        printf("FAIL  %-34s did not halt after %ld instructions "
               "(pc=%08x)\n", elf, steps, cpu.pc);
        printf("        mcause=%u mepc=%08x mtval=%08x mtvec=%08x\n",
               cpu.mcause, cpu.mepc, cpu.mtval, cpu.mtvec);
        for (k = n > 6 ? n - 6 : 0; k < n; k++) {
            const rv_trace_event_t *ev = rv_trace_peek(&cpu.trace, k);

            printf("        trace pc=%08x type=%u insn=%08x addr=%08x\n",
                   ev->pc, ev->type, ev->insn, ev->addr);
        }
        free(b.ram);
        return 1;
    }

    our_len = filter_hex(b.out, b.out_len, ours);
    ref_len = reference_output(elf, theirs);

    if (ref_len == 0) {
        printf("FAIL  %-34s the reference produced no signature\n", elf);
        rc = 1;
    } else if (our_len != ref_len || memcmp(ours, theirs, our_len) != 0) {
        uint32_t line = 0, oi = 0, ti = 0, shown = 0;

        printf("FAIL  %-34s signature mismatch\n", elf);
        while ((oi < our_len || ti < ref_len) && shown < 6) {
            char ob[16] = "--------", tb[16] = "--------";
            uint32_t k;

            for (k = 0; k < 8 && oi < our_len && ours[oi] != '\n'; k++)
                ob[k] = ours[oi++];
            for (k = 0; k < 8 && ti < ref_len && theirs[ti] != '\n'; k++)
                tb[k] = theirs[ti++];
            if (oi < our_len)
                oi++;
            if (ti < ref_len)
                ti++;
            if (memcmp(ob, tb, 8) != 0) {
                printf("        word %u ours %.8s qemu %.8s\n", line, ob, tb);
                shown++;
            }
            line++;
        }
        printf("        %u words ours, %u words qemu\n", our_len / 9,
               ref_len / 9);
        rc = 1;
    } else {
        printf("ok    %-34s %u signature words, %ld instructions\n", elf,
               our_len / 9, steps);
    }

    free(theirs);
    free(ours);
    free(b.out);
    free(b.ram);
    return rc;
}
