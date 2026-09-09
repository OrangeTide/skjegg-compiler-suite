/* icov-main.c : run a guest and report which instructions it executed */
/* Copyright (c) 2026 Jon Mayo - MIT-0 OR Public Domain */

/* Built only by icov-by-method.sh. It is test_harness.c with the checking
 * removed and the instruction-coverage report added, so that any compiled
 * guest can be turned into a set of instructions it exercised. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rv32.h"
#include "machine.h"
#include "elf_loader.h"
#include "icov.h"

#define MAX_STEPS  400000000

int
main(int argc, char **argv)
{
    rv_cpu cpu;
    machine mach;
    uint32_t entry;
    long steps = 0;
    int dump = 0, i;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <guest.elf> [-dump]\n", argv[0]);
        return 2;
    }
    for (i = 2; i < argc; i++)
        if (strcmp(argv[i], "-dump") == 0)
            dump = 1;

    if (machine_init(&mach, &cpu))
        return 1;
    mach.quiet = 1;

    entry = elf_load(argv[1], machine_poke, &mach);
    if (!entry) {
        machine_free(&mach);
        return 1;
    }
    rv_reset(&cpu, entry);
    rv_icov_reset();

    while (steps < MAX_STEPS && !mach.exited) {
        if (rv_step(&cpu) < 0)
            break;
        steps++;
    }

    if (dump)
        rv_icov_dump(stdout);
    else
        rv_icov_report(stdout, 1);

    machine_free(&mach);
    return 0;
}
