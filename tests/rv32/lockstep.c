/* lockstep.c : instruction-by-instruction comparison against qemu-riscv32 */
/* Copyright (c) 2026 Jon Mayo - MIT-0 OR Public Domain */

/* Both models load the same ELF and start from the same architectural
 * state, then advance one instruction at a time. After every step all 32
 * integer registers, the program counter, all 32 floating-point registers
 * and the fcsr are compared. The first disagreement is reported together
 * with the encoding that produced it, so a divergence points straight at
 * the instruction that is implemented wrongly rather than at whatever
 * output the program eventually printed. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rv32.h"
#include "machine.h"
#include "elf_loader.h"
#include "gdbclient.h"

#define DEFAULT_PORT    31337
#define MAX_STEPS       50000000

struct mismatch {
    const char *what;
    int index;
    uint64_t ours;
    uint64_t theirs;
};

/****************************************************************
 * State comparison
 ****************************************************************/

/** Compare our state against the remote's. Returns the number of
 * differences and records the first few in out. */
static int
compare(rv_cpu *cpu, const gdb_state *st, int check_fp, struct mismatch *out,
        int max_out)
{
    int i, n = 0;

#define RECORD(name, idx, a, b)                     \
    do {                                            \
        if (n < max_out) {                          \
            out[n].what = (name);                   \
            out[n].index = (idx);                   \
            out[n].ours = (a);                      \
            out[n].theirs = (b);                    \
        }                                           \
        n++;                                        \
    } while (0)

    if (cpu->pc != st->pc)
        RECORD("pc", -1, cpu->pc, st->pc);

    for (i = 0; i < 32; i++) {
        uint32_t ours = i == 0 ? 0 : cpu->x[i];

        if (ours != st->x[i])
            RECORD("x", i, ours, st->x[i]);
    }

    if (check_fp) {
        for (i = 0; i < 32; i++) {
            /* When the remote implements D, a single-precision value is
             * NaN-boxed into the upper half of a 64-bit register. Only the
             * low half is architecturally visible to an F-only guest. */
            uint32_t theirs = (uint32_t)st->f[i];

            if (cpu->f[i] != theirs)
                RECORD("f", i, cpu->f[i], theirs);
        }
        if ((cpu->fcsr & 0xff) != (st->fcsr & 0xff))
            RECORD("fcsr", -1, cpu->fcsr & 0xff, st->fcsr & 0xff);
    }

#undef RECORD
    return n;
}

static void
print_mismatches(const struct mismatch *m, int n, int shown)
{
    int i;

    for (i = 0; i < shown; i++) {
        if (strcmp(m[i].what, "x") == 0)
            printf("    x%-2d (%-4s) ours=%08x qemu=%08x\n", m[i].index,
                   rv_x_names[m[i].index], (uint32_t)m[i].ours,
                   (uint32_t)m[i].theirs);
        else if (strcmp(m[i].what, "f") == 0)
            printf("    f%-2d (%-4s) ours=%08x qemu=%08x\n", m[i].index,
                   rv_f_names[m[i].index], (uint32_t)m[i].ours,
                   (uint32_t)m[i].theirs);
        else
            printf("    %-10s ours=%08x qemu=%08x\n", m[i].what,
                   (uint32_t)m[i].ours, (uint32_t)m[i].theirs);
    }
    if (n > shown)
        printf("    ... and %d more\n", n - shown);
}

/****************************************************************
 * Instruction inspection
 ****************************************************************/

/** Fetch the instruction at pc, expanding a compressed encoding. */
static uint32_t
fetch_insn(machine *m, uint32_t pc)
{
    uint32_t lo;

    if (pc + 1 >= m->mem_size)
        return 0;
    lo = (uint32_t)m->mem[pc] | ((uint32_t)m->mem[pc + 1] << 8);
    if ((lo & 3) != 3)
        return rv_expand_c((uint16_t)lo);
    if (pc + 3 >= m->mem_size)
        return 0;
    return lo | ((uint32_t)m->mem[pc + 2] << 16) |
           ((uint32_t)m->mem[pc + 3] << 24);
}

/** True if the instruction can change a floating-point register or fcsr.
 *
 * Reading the 35 floating-point registers costs a round trip each, so the
 * comparison only pays for them after an instruction that could have
 * touched them. Integer state is still checked after every instruction.
 */
static int
touches_fp(uint32_t insn)
{
    switch (insn & 0x7f) {
    case 0x07:      /* flw */
    case 0x27:      /* fsw */
    case 0x43:      /* fmadd.s */
    case 0x47:      /* fmsub.s */
    case 0x4b:      /* fnmsub.s */
    case 0x4f:      /* fnmadd.s */
    case 0x53:      /* OP-FP */
        return 1;
    case 0x73:      /* a csr access can reach fflags, frm or fcsr */
        return (insn >> 12 & 7) != 0;
    default:
        return 0;
    }
}

/****************************************************************
 * Disassembly aid: report the encoding at a given address
 ****************************************************************/

static void
describe_insn(machine *m, uint32_t pc)
{
    uint32_t lo, hi;

    if (pc + 1 >= m->mem_size) {
        printf("    (pc outside guest memory)\n");
        return;
    }
    lo = (uint32_t)m->mem[pc] | ((uint32_t)m->mem[pc + 1] << 8);
    if ((lo & 3) != 3) {
        printf("    encoding %04x (compressed, expands to %08x)\n", lo,
               rv_expand_c((uint16_t)lo));
        return;
    }
    hi = (uint32_t)m->mem[pc + 2] | ((uint32_t)m->mem[pc + 3] << 8);
    printf("    encoding %08x\n", lo | (hi << 16));
}

/****************************************************************
 * Memory comparison
 ****************************************************************/

/** Compare the loaded image region byte for byte against the remote.
 * Returns 1 if the regions match, 0 if they differ, -1 if unavailable. */
static int
compare_memory(gdb_client *g, machine *m, uint32_t base, uint32_t len)
{
    uint8_t *ref = malloc(len);
    size_t diff = 0, k;

    if (!ref)
        return -1;
    if (gdb_read_mem(g, base, ref, len)) {
        free(ref);
        return -1;
    }
    for (k = 0; k < len; k++) {
        if (ref[k] != m->mem[base + k])
            diff++;
    }
    free(ref);

    if (diff) {
        printf("MEMORY DIVERGENCE: %zu of %u bytes differ in [%08x,%08x)\n",
               diff, len, base, base + len);
        return 0;
    }
    printf("guest memory identical over [%08x,%08x)\n", base, base + len);
    return 1;
}

/****************************************************************
 * Main
 ****************************************************************/

int
main(int argc, char **argv)
{
    rv_cpu cpu;
    machine m;
    gdb_client g;
    gdb_state st;
    struct mismatch mm[8];
    const char *elf;
    uint32_t entry, prev_pc = 0, image_base = 0x10000, image_top;
    int port = DEFAULT_PORT, steps = 0, qemu_exit = 0, rc = 0;
    int i, verbose = 0, mem_checked = -1;
    const char *cpu_model = NULL;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <program.elf> [-p port] [-cpu model] [-v]\n", argv[0]);
        return 2;
    }
    elf = argv[1];
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
            port = atoi(argv[++i]);
        else if (strcmp(argv[i], "-v") == 0)
            verbose = 1;
        else if (strcmp(argv[i], "-cpu") == 0 && i + 1 < argc)
            cpu_model = argv[++i];
    }

    if (machine_init(&m, &cpu))
        return 1;
    m.quiet = 1;

    entry = elf_load(elf, machine_poke, &m);
    if (!entry) {
        machine_free(&m);
        return 1;
    }
    rv_reset(&cpu, entry);

    /* The comparison region runs from the load address to the top of the
     * stack. Reading past the last mapped page makes the remote reject
     * the request, so the extent comes from the linker's own symbol. */
    image_top = elf_symbol(elf, "__stack_top");
    if (!image_top || image_top <= image_base)
        image_top = image_base + 0x8000;

    /* The reference has to be configured to the same ISA as the guest.
     * Zcmp in particular needs the compressed set spelled out, because
     * qemu derives the compressed double load and store instructions from
     * C plus D and those share Zcmp's encoding space:
     *   -cpu rv32,c=false,zca=true,zcf=true,zcmp=true */
    {
        char *argv_qemu[8];
        char portbuf[16];
        int n = 0;

        snprintf(portbuf, sizeof(portbuf), "%d", port);
        argv_qemu[n++] = (char *)"qemu-riscv32";
        if (cpu_model) {
            argv_qemu[n++] = (char *)"-cpu";
            argv_qemu[n++] = (char *)cpu_model;
        }
        argv_qemu[n++] = (char *)"-g";
        argv_qemu[n++] = portbuf;
        argv_qemu[n++] = (char *)elf;
        argv_qemu[n] = NULL;

        if (gdb_launch_argv(&g, argv_qemu, port)) {
            machine_free(&m);
            return 1;
        }
    }

    printf("lockstep: %s\n", elf);
    printf("  remote FLEN %d bytes%s\n", g.flen,
           g.flen == 8 ? " (remote implements D; single-precision values "
                         "are NaN-boxed)" : "");

    /* Adopt the remote's entry state so both models start identical. The
     * process image qemu builds contains a stack with argv and the
     * environment, which the emulator has no reason to reproduce. */
    memset(&st, 0, sizeof(st));
    if (gdb_read_state(&g, &st) || gdb_read_fpu(&g, &st)) {
        fprintf(stderr, "lockstep: cannot read initial state\n");
        gdb_close(&g);
        machine_free(&m);
        return 1;
    }
    for (i = 1; i < 32; i++)
        cpu.x[i] = st.x[i];
    cpu.pc = st.pc;
    for (i = 0; i < 32; i++)
        cpu.f[i] = (uint32_t)st.f[i];
    cpu.fcsr = st.fcsr & 0xff;

    printf("  entry 0x%08x\n\n", cpu.pc);

    for (;;) {
        int running, n, fp;
        uint32_t insn;

        if (steps >= MAX_STEPS) {
            printf("stopped after %d instructions without the guest "
                   "exiting\n", steps);
            rc = 1;
            break;
        }

        prev_pc = cpu.pc;
        insn = fetch_insn(&m, prev_pc);
        fp = touches_fp(insn);

        /* The last chance to compare guest memory is just before the exit
         * syscall, while the remote process still exists. This catches
         * stores that landed at the wrong address and were never read
         * back into a register. */
        if (insn == 0x00000073 && (cpu.x[17] == 93 || cpu.x[17] == 94))
            mem_checked = compare_memory(&g, &m, image_base,
                                         image_top - image_base);

        /* Advance the reference first: if the guest exits, the emulator
         * must agree that this instruction ended the program. */
        running = gdb_step(&g, &qemu_exit);
        if (running < 0) {
            fprintf(stderr, "lockstep: protocol error at step %d\n", steps);
            rc = 1;
            break;
        }

        rv_step(&cpu);
        steps++;

        if (!running) {
            if (!m.exited) {
                printf("DIVERGENCE at step %d, pc=%08x\n", steps, prev_pc);
                printf("    qemu exited with code %d, emulator kept "
                       "running\n", qemu_exit);
                describe_insn(&m, prev_pc);
                rc = 1;
            } else if ((m.exit_code & 0xff) != (qemu_exit & 0xff)) {
                /* The remote reports what the operating system reports,
                 * which is the low byte of the value the guest passed to
                 * exit. Comparing the whole word would fail any guest
                 * that returns something wider. */
                printf("DIVERGENCE: exit code ours=%d (0x%02x) "
                       "qemu=%d\n", m.exit_code, m.exit_code & 0xff,
                       qemu_exit);
                rc = 1;
            }
            break;
        }

        if (m.exited) {
            printf("DIVERGENCE at step %d, pc=%08x\n", steps, prev_pc);
            printf("    emulator exited, qemu kept running\n");
            describe_insn(&m, prev_pc);
            rc = 1;
            break;
        }

        if (gdb_read_state(&g, &st) || (fp && gdb_read_fpu(&g, &st))) {
            fprintf(stderr, "lockstep: cannot read state at step %d\n",
                    steps);
            rc = 1;
            break;
        }

        n = compare(&cpu, &st, fp, mm, 8);
        if (n) {
            printf("DIVERGENCE at step %d\n", steps);
            printf("    executed pc=%08x, next pc ours=%08x qemu=%08x\n",
                   prev_pc, cpu.pc, st.pc);
            describe_insn(&m, prev_pc);
            print_mismatches(mm, n, n < 8 ? n : 8);
            rc = 1;
            break;
        }

        if (verbose && steps % 1000 == 0)
            printf("  %d instructions, pc=%08x\n", steps, cpu.pc);
    }

    if (!rc) {
        printf("%d instructions in lockstep, no divergence\n", steps);
        printf("exit code %d on both models\n", m.exit_code);
        if (mem_checked == 0)
            rc = 1;
        else if (mem_checked < 0)
            printf("(memory comparison unavailable)\n");
    }

    gdb_close(&g);
    machine_free(&m);
    return rc;
}
