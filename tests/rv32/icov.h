/* icov.h : guest-instruction coverage */
/* Copyright (c) 2026 Jon Mayo - MIT-0 OR Public Domain */

#ifndef ICOV_H
#define ICOV_H

#include <stdint.h>
#include <stdio.h>

/* rv32.c calls this once per executed instruction when built with
 * -DRV_ICOV. len is 2 for a compressed encoding and 4 otherwise, and insn
 * is the encoding as fetched rather than as expanded, so that a compressed
 * form counts as itself. */
void rv_icov_note(uint32_t insn, int len);

void rv_icov_reset(void);
int  rv_icov_total(void);
int  rv_icov_seen(void);

/* A one-line summary, optionally followed by the instructions that were
 * never executed. */
void rv_icov_report(FILE *out, int list_missing);

/* Every instruction and its count, one per line, for set arithmetic
 * between methods. */
void rv_icov_dump(FILE *out);

#endif /* ICOV_H */
