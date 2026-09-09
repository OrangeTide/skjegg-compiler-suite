/* machine.h : flat memory and syscall environment for the RV32 emulator */
/* Copyright (c) 2026 Jon Mayo - MIT-0 OR Public Domain */

#ifndef MACHINE_H
#define MACHINE_H

#include "rv32.h"

/* Guest physical memory: a flat window at address zero. Test programs link
 * at 0x00010000, which is also where qemu-riscv32 maps them, so the same
 * binary runs unmodified under both models. */
#define MACHINE_MEM_SIZE  (16u * 1024u * 1024u)

typedef struct machine {
    uint8_t *mem;
    uint32_t mem_size;
    int      exited;        /* guest called exit */
    int      exit_code;
    int      quiet;         /* suppress guest console output */
    uint64_t syscalls;
} machine;

/* Allocate memory and wire the cpu's bus callbacks and ecall handler. */
int machine_init(machine *m, rv_cpu *cpu);
void machine_free(machine *m);

/* Byte write callback in the shape the ELF loader expects. */
void machine_poke(void *ctx, uint32_t addr, uint8_t byte);

#endif /* MACHINE_H */
