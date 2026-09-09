/* test_harness.c : runs a freestanding RV32 ELF on the emulator */
/* Copyright (c) 2026 Jon Mayo - MIT-0 OR Public Domain */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rv32.h"
#include "machine.h"
#include "elf_loader.h"

/* High enough to let a real application run to completion. CoreMark's
 * default workload is tens of millions of instructions. */
#define MAX_STEPS 2000000000

/****************************************************************
 * Trace reporting
 ****************************************************************/

static const char *
trace_name(int type)
{
    switch (type) {
    case RV_TR_ILLEGAL:        return "illegal";
    case RV_TR_FETCH_FAULT:    return "fetch-fault";
    case RV_TR_LOAD_FAULT:     return "load-fault";
    case RV_TR_STORE_FAULT:    return "store-fault";
    case RV_TR_LOAD_MISALIGN:  return "load-misaligned";
    case RV_TR_STORE_MISALIGN: return "store-misaligned";
    case RV_TR_ECALL:          return "ecall";
    case RV_TR_EBREAK:         return "ebreak";
    case RV_TR_CSR:            return "csr";
    case RV_TR_TRAP:           return "trap";
    case RV_TR_DOUBLE_FAULT:   return "double-fault";
    default:                   return "?";
    }
}

static void
dump_trace(rv_cpu *cpu)
{
    uint32_t n = rv_trace_count(&cpu->trace);
    uint32_t i;

    if (n == 0)
        return;
    printf("\nlast %u trace events%s:\n", n,
           rv_trace_overflowed(&cpu->trace) ? " (ring wrapped)" : "");
    for (i = 0; i < n; i++) {
        const rv_trace_event_t *ev = rv_trace_peek(&cpu->trace, i);

        printf("  pc=%08x %-16s insn=%08x addr=%08x %s\n",
               ev->pc, trace_name(ev->type), ev->insn, ev->addr, ev->note);
    }
}

/****************************************************************
 * Expected results
 ****************************************************************/

struct test_case {
    const char *symbol;
    uint32_t expect;
};

static const struct test_case cases[] = {
    { "result_fib",    55 },
    { "result_gcd",    21 },
    { "result_sum",    5050 },
    { "result_bits",   0x0A55 },
    { "result_sqrt_i", 1414 },
};

int
main(int argc, char **argv)
{
    rv_cpu cpu;
    machine m;
    uint32_t entry;
    int steps, failures = 0;
    size_t i;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <program.elf>\n", argv[0]);
        return 2;
    }

    if (machine_init(&m, &cpu))
        return 1;

    entry = elf_load(argv[1], machine_poke, &m);
    if (!entry) {
        machine_free(&m);
        return 1;
    }
    rv_reset(&cpu, entry);

    printf("loaded %s, entry 0x%08x\n\n", argv[1], entry);

    steps = rv_run(&cpu, MAX_STEPS);

    printf("\n");
    if (!m.exited) {
        printf("program did not exit cleanly after %d instructions\n", steps);
        dump_trace(&cpu);
        machine_free(&m);
        return 1;
    }

    /* A program that carries none of the result slots is not the bundled
     * test program but something else being run for its own sake, so
     * report what it did rather than looking for results it never had. */
    if (!elf_symbol(argv[1], cases[0].symbol)) {
        printf("%d instructions retired, %llu syscalls, exit code %d\n",
               steps, (unsigned long long)m.syscalls, m.exit_code);
        machine_free(&m);
        return m.exit_code != 0;
    }

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint32_t addr = elf_symbol(argv[1], cases[i].symbol);
        uint32_t got;

        if (!addr) {
            printf("  MISSING %s\n", cases[i].symbol);
            failures++;
            continue;
        }
        got = (uint32_t)m.mem[addr] | ((uint32_t)m.mem[addr + 1] << 8) |
              ((uint32_t)m.mem[addr + 2] << 16) |
              ((uint32_t)m.mem[addr + 3] << 24);
        if (got != cases[i].expect) {
            printf("  FAIL %-14s got %u want %u\n", cases[i].symbol, got,
                   cases[i].expect);
            failures++;
        } else {
            printf("  ok   %-14s %u\n", cases[i].symbol, got);
        }
    }

    printf("\n%d instructions retired, %llu syscalls, exit code %d\n",
           steps, (unsigned long long)m.syscalls, m.exit_code);

    if (m.exit_code != 0) {
        printf("guest reported %d failing tests\n", m.exit_code);
        failures += m.exit_code;
    }

    if (failures) {
        printf("FAILED: %d\n", failures);
        dump_trace(&cpu);
    } else {
        printf("all harness checks passed\n");
    }

    machine_free(&m);
    return failures ? 1 : 0;
}
