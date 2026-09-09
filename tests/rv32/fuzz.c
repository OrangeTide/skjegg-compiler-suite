/* fuzz.c : randomized instruction comparison against qemu-riscv32 */
/* Copyright (c) 2026 Jon Mayo - MIT-0 OR Public Domain */

/* Compiled programs only reach the instruction encodings and operand
 * values a compiler chooses to emit. This tool reaches the rest: it fills
 * a buffer with random valid encodings, randomizes every architectural
 * register including deliberately awkward floating-point values, and then
 * steps both models one instruction at a time comparing the result.
 *
 * The generated stream deliberately excludes control flow and memory
 * access. Both are covered by the compiled test programs and by the
 * compliance suite, and excluding them keeps the program counter
 * advancing linearly so that a random encoding cannot wander off into
 * unmapped memory and end the run. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "rv32.h"
#include "machine.h"
#include "elf_loader.h"
#include "gdbclient.h"

#define DEFAULT_PORT    31338
#define BLOCK_INSNS     512     /* random instructions per round */
#define FULL_CHECK      64      /* full register sweep every N steps */
#define CODE_BUF_SIZE   262144  /* must match fuzz_target.S */

/* qemu-riscv32 write-protects a page once it has translated code from it,
 * so a rewritten block at the same address is refused. Each round takes a
 * fresh slice of the buffer, and the reference process is relaunched when
 * the buffer runs out. */

/****************************************************************
 * Random number generator
 *
 * A local xorshift keeps runs reproducible from a seed regardless of the
 * host C library.
 ****************************************************************/

static uint64_t rng_state = 0x853c49e6748fea9bull;

static uint32_t
rnd(void)
{
    uint64_t x = rng_state;

    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return (uint32_t)(x >> 32);
}

static uint32_t
rnd_below(uint32_t n)
{
    return rnd() % n;
}

/****************************************************************
 * Interesting operand values
 ****************************************************************/

static const uint32_t float_specials[] = {
    0x00000000,     /* +0 */
    0x80000000,     /* -0 */
    0x3f800000,     /* 1.0 */
    0xbf800000,     /* -1.0 */
    0x7f800000,     /* +inf */
    0xff800000,     /* -inf */
    0x7fc00000,     /* canonical quiet NaN */
    0x7fa00000,     /* signalling NaN */
    0xffa00000,     /* negative signalling NaN */
    0x00000001,     /* smallest subnormal */
    0x807fffff,     /* largest negative subnormal */
    0x00800000,     /* smallest normal */
    0x7f7fffff,     /* largest finite */
    0xff7fffff,
    0x33800000,     /* 2^-24, a rounding boundary against 1.0 */
    0x4b800000,     /* 2^24, where consecutive integers stop being exact */
    0x4b7fffff,
    0x40490fdb,     /* pi */
    0x3eaaaaab,     /* 1/3 */
    0x00000002,
};

static uint32_t
rnd_float_bits(void)
{
    switch (rnd_below(4)) {
    case 0:
        return float_specials[rnd_below(sizeof(float_specials) /
                                        sizeof(float_specials[0]))];
    case 1:
        /* A small integer value, where results are usually exact */
        return (uint32_t)(rnd_below(64) << 23) | 0x30000000u;
    case 2:
        /* Random bits within the normal exponent range */
        return (rnd() & 0x807fffffu) | ((0x40 + rnd_below(0x60)) << 23);
    default:
        return rnd();
    }
}

static uint32_t
rnd_int_value(void)
{
    switch (rnd_below(4)) {
    case 0:
        return 0;
    case 1:
        return (uint32_t)(int32_t)-1;
    case 2:
        return 0x80000000u;         /* the signed overflow operand */
    default:
        return rnd();
    }
}

/****************************************************************
 * Instruction generation
 *
 * Every generated encoding is architecturally valid. Reserved rounding
 * modes and illegal encodings are covered by directed unit tests instead:
 * they would make the remote take a signal and end the process.
 ****************************************************************/

static uint32_t
rnd_rm(void)
{
    static const uint32_t modes[] = { 0, 1, 2, 3, 4, 7 };

    return modes[rnd_below(6)];
}

static uint32_t
gen_op_imm(void)
{
    uint32_t rd = rnd_below(32), rs1 = rnd_below(32);
    uint32_t f3 = rnd_below(8);
    uint32_t imm;

    if (f3 == 1) {                          /* slli */
        imm = rnd_below(32);
    } else if (f3 == 5) {                   /* srli / srai */
        imm = rnd_below(32) | (rnd_below(2) ? 0x400 : 0);
    } else {
        imm = rnd() & 0xfff;
    }
    return (imm << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | 0x13;
}

static uint32_t
gen_op_reg(void)
{
    uint32_t rd = rnd_below(32), rs1 = rnd_below(32), rs2 = rnd_below(32);
    uint32_t f3, f7;

    if (rnd_below(2)) {                     /* M extension */
        f7 = 0x01;
        f3 = rnd_below(8);
    } else {
        f3 = rnd_below(8);
        f7 = (f3 == 0 || f3 == 5) && rnd_below(2) ? 0x20 : 0x00;
    }
    return (f7 << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) |
           (rd << 7) | 0x33;
}

/** Generate a Zba, Zbb or Zbs register-register encoding.
 *
 * These are worth fuzzing rather than only unit testing because the
 * generator reaches shift amounts and bit indices a compiler never emits:
 * a rotate by zero, which in a careless implementation shifts by 32 and is
 * undefined in C, and a bit index above 31, which has to wrap.
 */
static uint32_t
gen_zb_reg(void)
{
    static const struct { uint32_t f7, f3; } forms[] = {
        { 0x10, 2 }, { 0x10, 4 }, { 0x10, 6 },  /* sh1add, sh2add, sh3add */
        { 0x20, 7 }, { 0x20, 6 }, { 0x20, 4 },  /* andn, orn, xnor */
        { 0x05, 4 }, { 0x05, 5 },               /* min, minu */
        { 0x05, 6 }, { 0x05, 7 },               /* max, maxu */
        { 0x30, 1 }, { 0x30, 5 },               /* rol, ror */
        { 0x14, 1 }, { 0x24, 1 },               /* bset, bclr */
        { 0x34, 1 }, { 0x24, 5 },               /* binv, bext */
    };
    uint32_t rd = rnd_below(32), rs1 = rnd_below(32), rs2 = rnd_below(32);
    uint32_t i;

    /* zext.h is the one register-register form that fixes its rs2 field */
    if (rnd_below(16) == 0)
        return (0x04u << 25) | (rs1 << 15) | (4u << 12) | (rd << 7) | 0x33;

    i = rnd_below(sizeof(forms) / sizeof(forms[0]));
    return (forms[i].f7 << 25) | (rs2 << 20) | (rs1 << 15) |
           (forms[i].f3 << 12) | (rd << 7) | 0x33;
}

/** Generate a Zbb or Zbs register-immediate encoding. The unary Zbb
 * instructions reuse the shift-amount field as a selector, so the shift
 * amount is only free for the forms that really take one. */
static uint32_t
gen_zb_imm(void)
{
    static const uint32_t unary[] = { 0, 1, 2, 4, 5 };
    uint32_t rd = rnd_below(32), rs1 = rnd_below(32);
    uint32_t f7, f3, shamt = rnd_below(32);

    switch (rnd_below(9)) {
    case 0: f7 = 0x14; f3 = 1; break;                   /* bseti */
    case 1: f7 = 0x24; f3 = 1; break;                   /* bclri */
    case 2: f7 = 0x34; f3 = 1; break;                   /* binvi */
    case 3: f7 = 0x24; f3 = 5; break;                   /* bexti */
    case 4: case 5: f7 = 0x30; f3 = 5; break;           /* rori */
    case 6: f7 = 0x14; f3 = 5; shamt = 0x07; break;     /* orc.b */
    case 7: f7 = 0x34; f3 = 5; shamt = 0x18; break;     /* rev8 */
    default:                                            /* clz, ctz, cpop,
                                                           sext.b, sext.h */
        f7 = 0x30;
        f3 = 1;
        shamt = unary[rnd_below(sizeof(unary) / sizeof(unary[0]))];
        break;
    }
    return (f7 << 25) | (shamt << 20) | (rs1 << 15) | (f3 << 12) |
           (rd << 7) | 0x13;
}

static uint32_t
gen_upper(void)
{
    uint32_t rd = rnd_below(32);

    return (rnd() & 0xfffff000u) | (rd << 7) | (rnd_below(2) ? 0x37 : 0x17);
}

static uint32_t
gen_op_fp(void)
{
    uint32_t rd = rnd_below(32), rs1 = rnd_below(32), rs2 = rnd_below(32);
    uint32_t f7, f3;

    switch (rnd_below(11)) {
    case 0: f7 = 0x00; f3 = rnd_rm(); break;            /* fadd.s */
    case 1: f7 = 0x04; f3 = rnd_rm(); break;            /* fsub.s */
    case 2: f7 = 0x08; f3 = rnd_rm(); break;            /* fmul.s */
    case 3: f7 = 0x0c; f3 = rnd_rm(); break;            /* fdiv.s */
    case 4: f7 = 0x2c; f3 = rnd_rm(); rs2 = 0; break;   /* fsqrt.s */
    case 5: f7 = 0x10; f3 = rnd_below(3); break;        /* fsgnj family */
    case 6: f7 = 0x14; f3 = rnd_below(2); break;        /* fmin / fmax */
    case 7: f7 = 0x50; f3 = rnd_below(3); break;        /* compares */
    case 8: f7 = 0x60; f3 = rnd_rm(); rs2 = rnd_below(2); break; /* to int */
    case 9: f7 = 0x68; f3 = rnd_rm(); rs2 = rnd_below(2); break; /* from int */
    default:
        if (rnd_below(2)) {
            f7 = 0x70; f3 = rnd_below(2); rs2 = 0;      /* fmv.x.w/fclass */
        } else {
            f7 = 0x78; f3 = 0; rs2 = 0;                 /* fmv.w.x */
        }
        break;
    }
    return (f7 << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) |
           (rd << 7) | 0x53;
}

static uint32_t
gen_fma(void)
{
    static const uint32_t ops[] = { 0x43, 0x47, 0x4b, 0x4f };
    uint32_t rd = rnd_below(32), rs1 = rnd_below(32);
    uint32_t rs2 = rnd_below(32), rs3 = rnd_below(32);

    return (rs3 << 27) | (rs2 << 20) | (rs1 << 15) | (rnd_rm() << 12) |
           (rd << 7) | ops[rnd_below(4)];
}

/** Generate a compressed arithmetic encoding, avoiding control flow,
 * memory access and the reserved forms. */
static uint16_t
gen_compressed(void)
{
    uint32_t rd, rs1p, rs2p, imm;

    switch (rnd_below(11)) {
    case 0:     /* c.addi */
        rd = 1 + rnd_below(31);
        imm = rnd_below(64);
        return (uint16_t)(0x0001 | ((imm & 0x1f) << 2) |
                          ((imm >> 5 & 1) << 12) | (rd << 7));
    case 1:     /* c.li */
        rd = 1 + rnd_below(31);
        imm = rnd_below(64);
        return (uint16_t)(0x4001 | ((imm & 0x1f) << 2) |
                          ((imm >> 5 & 1) << 12) | (rd << 7));
    case 2:     /* c.lui, excluding the reserved rd and zero immediate */
        rd = 3 + rnd_below(29);
        if (rd == 2)
            rd = 3;
        imm = 1 + rnd_below(63);
        return (uint16_t)(0x6001 | ((imm & 0x1f) << 2) |
                          ((imm >> 5 & 1) << 12) | (rd << 7));
    case 3:     /* c.slli */
        rd = 1 + rnd_below(31);
        return (uint16_t)(0x0002 | (rnd_below(32) << 2) | (rd << 7));
    case 4:     /* c.srli / c.srai */
        rs1p = rnd_below(8);
        return (uint16_t)(0x8001 | (rnd_below(32) << 2) | (rs1p << 7) |
                          (rnd_below(2) << 10));
    case 5:     /* c.andi */
        rs1p = rnd_below(8);
        imm = rnd_below(64);
        return (uint16_t)(0x8801 | ((imm & 0x1f) << 2) |
                          ((imm >> 5 & 1) << 12) | (rs1p << 7));
    case 6:     /* c.sub / c.xor / c.or / c.and */
        rs1p = rnd_below(8);
        rs2p = rnd_below(8);
        return (uint16_t)(0x8c01 | (rs2p << 2) | (rnd_below(4) << 5) |
                          (rs1p << 7));
    case 7:     /* c.mv */
        rd = 1 + rnd_below(31);
        return (uint16_t)(0x8002 | ((1 + rnd_below(31)) << 2) | (rd << 7));
    case 8:     /* c.add */
        rd = 1 + rnd_below(31);
        return (uint16_t)(0x9002 | ((1 + rnd_below(31)) << 2) | (rd << 7));
    case 9: {   /* Zcb: the one-operand forms */
        /* Selector 4 is c.zext.w, which is RV64 only, and 6 and 7 are
         * reserved. Generating those would stop the reference process
         * rather than test anything. */
        static const uint32_t sel[] = { 0, 1, 2, 3, 5 };

        rs1p = rnd_below(8);
        return (uint16_t)(0x9c01 | (rs1p << 7) | (3u << 5) |
                          (sel[rnd_below(5)] << 2));
    }
    default:    /* Zcb: c.mul */
        rs1p = rnd_below(8);
        rs2p = rnd_below(8);
        return (uint16_t)(0x9c01 | (rs1p << 7) | (2u << 5) | (rs2p << 2));
    }
}

/****************************************************************
 * The reference process
 ****************************************************************/

/* qemu's default rv32 model happens to include Zba, Zbb and Zbs, but that
 * is a default and not a promise. Spelling the model out pins the
 * reference to the ISA this emulator implements, so a later qemu that
 * changes its default cannot quietly turn the bit-manipulation comparison
 * into an illegal-instruction signal. Pass -cpu to override: running with
 * zba=false,zbb=false,zbs=false is a quick check that the generator really
 * is producing these encodings. */
static const char *cpu_model = "rv32,zba=true,zbb=true,zbs=true,zcb=true";

static int
launch_reference(gdb_client *g, const char *elf, int port)
{
    char *argv_qemu[8];
    char portbuf[16];
    int n = 0;

    snprintf(portbuf, sizeof(portbuf), "%d", port);
    argv_qemu[n++] = (char *)"qemu-riscv32";
    if (cpu_model && *cpu_model) {
        argv_qemu[n++] = (char *)"-cpu";
        argv_qemu[n++] = (char *)cpu_model;
    }
    argv_qemu[n++] = (char *)"-g";
    argv_qemu[n++] = portbuf;
    argv_qemu[n++] = (char *)elf;
    argv_qemu[n] = NULL;

    return gdb_launch_argv(g, argv_qemu, port);
}

/****************************************************************
 * Reporting
 ****************************************************************/

static void
describe(uint32_t insn, int len)
{
    if (len == 2) {
        uint32_t full = rv_expand_zcb((uint16_t)insn);

        if (full == 0)
            full = rv_expand_c((uint16_t)insn);
        printf("  encoding %04x (compressed, expands to %08x)\n",
               insn & 0xffff, full);
    }
    else
        printf("  encoding %08x  opcode=%02x rd=%u rs1=%u rs2=%u "
               "funct3=%u funct7=%02x\n",
               insn, insn & 0x7f, insn >> 7 & 0x1f, insn >> 15 & 0x1f,
               insn >> 20 & 0x1f, insn >> 12 & 7, insn >> 25 & 0x7f);
}

static void
dump_operands(const rv_cpu *before, uint32_t insn)
{
    uint32_t rs1 = insn >> 15 & 0x1f, rs2 = insn >> 20 & 0x1f;
    uint32_t rs3 = insn >> 27 & 0x1f;
    uint32_t opcode = insn & 0x7f;

    printf("  integer operands: x%u=%08x x%u=%08x\n", rs1, before->x[rs1],
           rs2, before->x[rs2]);
    if (opcode == 0x53 || opcode == 0x43 || opcode == 0x47 ||
        opcode == 0x4b || opcode == 0x4f) {
        printf("  float operands:   f%u=%08x f%u=%08x", rs1, before->f[rs1],
               rs2, before->f[rs2]);
        if (opcode != 0x53)
            printf(" f%u=%08x", rs3, before->f[rs3]);
        printf("\n  fcsr before:      %02x (frm=%u fflags=%02x)\n",
               before->fcsr & 0xff, before->fcsr >> 5 & 7,
               before->fcsr & 0x1f);
    }
}

/****************************************************************
 * Main
 ****************************************************************/

int
main(int argc, char **argv)
{
    rv_cpu cpu, before;
    machine m;
    gdb_client g;
    gdb_state st;
    const char *elf;
    uint32_t entry, code_base;
    uint8_t *block;
    uint8_t lens[BLOCK_INSNS];
    long rounds = 20, round;
    long total = 0, fp_total = 0, relaunches = 0;
    uint32_t cursor = 0;
    int port = DEFAULT_PORT, rc = 0, i, quiet = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <fuzz_target.elf> [-n rounds] "
                        "[-s seed] [-p port] [-cpu model] [-q]\n", argv[0]);
        return 2;
    }
    elf = argv[1];
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
            rounds = atol(argv[++i]);
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
            rng_state = strtoull(argv[++i], NULL, 0) | 1;
        else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
            port = atoi(argv[++i]);
        else if (strcmp(argv[i], "-cpu") == 0 && i + 1 < argc)
            cpu_model = argv[++i];
        else if (strcmp(argv[i], "-q") == 0)
            quiet = 1;
    }

    if (machine_init(&m, &cpu))
        return 1;
    m.quiet = 1;

    entry = elf_load(elf, machine_poke, &m);
    if (!entry) {
        machine_free(&m);
        return 1;
    }
    code_base = elf_symbol(elf, "fuzz_code");
    if (!code_base) {
        fprintf(stderr, "fuzz: %s has no fuzz_code symbol\n", elf);
        machine_free(&m);
        return 1;
    }
    rv_reset(&cpu, entry);

    if (launch_reference(&g, elf, port)) {
        machine_free(&m);
        return 1;
    }

    block = malloc(BLOCK_INSNS * 4);
    if (!block) {
        gdb_close(&g);
        machine_free(&m);
        return 1;
    }

    printf("fuzz: %ld rounds of %d instructions, code buffer at %08x\n\n",
           rounds, BLOCK_INSNS, code_base);

    for (round = 0; round < rounds && !rc; round++) {
        uint32_t len = 0, base;
        int n;

        /* Lay out a block of random encodings */
        for (n = 0; n < BLOCK_INSNS; n++) {
            if (rnd_below(100) < 25) {
                uint16_t c = gen_compressed();

                block[len++] = (uint8_t)c;
                block[len++] = (uint8_t)(c >> 8);
                lens[n] = 2;
            } else {
                uint32_t w;

                switch (rnd_below(12)) {
                case 0: case 1: w = gen_op_imm(); break;
                case 2: case 3: w = gen_op_reg(); break;
                case 4: w = gen_upper(); break;
                case 5: case 6: case 7: w = gen_op_fp(); break;
                case 8: case 9: w = gen_fma(); break;
                case 10: w = gen_zb_reg(); break;
                default: w = gen_zb_imm(); break;
                }
                block[len++] = (uint8_t)w;
                block[len++] = (uint8_t)(w >> 8);
                block[len++] = (uint8_t)(w >> 16);
                block[len++] = (uint8_t)(w >> 24);
                lens[n] = 4;
            }
        }

        /* Take a fresh slice, restarting the reference process when the
         * code buffer is used up. */
        if (cursor + 4096 > CODE_BUF_SIZE) {
            gdb_close(&g);
            if (launch_reference(&g, elf, port)) {
                fprintf(stderr, "fuzz: cannot relaunch the reference\n");
                rc = 1;
                break;
            }
            cursor = 0;
            relaunches++;
        }
        base = code_base + cursor;
        cursor += 4096;         /* a whole page: qemu protects by page */

        for (i = 0; i < (int)len; i++)
            m.mem[base + i] = block[i];
        if (gdb_write_mem(&g, base, block, len)) {
            fprintf(stderr, "fuzz: cannot write code buffer\n");
            rc = 1;
            break;
        }

        /* Randomize every architectural register on both models */
        memset(&st, 0, sizeof(st));
        for (i = 1; i < 32; i++)
            st.x[i] = rnd_int_value();
        st.pc = base;
        for (i = 0; i < 32; i++)
            st.f[i] = rnd_float_bits();
        st.fcsr = (rnd_below(5) << 5) | rnd_below(32);

        if (gdb_write_state(&g, &st) || gdb_write_fpu(&g, &st)) {
            fprintf(stderr, "fuzz: cannot set remote state\n");
            rc = 1;
            break;
        }
        memset(cpu.x, 0, sizeof(cpu.x));
        for (i = 1; i < 32; i++)
            cpu.x[i] = st.x[i];
        cpu.pc = st.pc;
        for (i = 0; i < 32; i++)
            cpu.f[i] = (uint32_t)st.f[i];
        cpu.fcsr = st.fcsr & 0xff;
        cpu.halted = 0;

        for (n = 0; n < BLOCK_INSNS && !rc; n++) {
            uint32_t insn, rd;
            uint64_t fq;
            uint32_t fcsr_q;
            int running, is_fp, full;

            insn = (uint32_t)m.mem[cpu.pc] |
                   ((uint32_t)m.mem[cpu.pc + 1] << 8);
            if (lens[n] == 4)
                insn |= ((uint32_t)m.mem[cpu.pc + 2] << 16) |
                        ((uint32_t)m.mem[cpu.pc + 3] << 24);

            before = cpu;

            running = gdb_step(&g, NULL);
            if (running <= 0) {
                fprintf(stderr, "fuzz: remote stopped unexpectedly at "
                                "round %ld instruction %d\n", round, n);
                describe(insn, lens[n]);
                rc = 1;
                break;
            }
            rv_step(&cpu);
            total++;

            if (cpu.in_trap || cpu.halted) {
                printf("DIVERGENCE round %ld instruction %d: the emulator "
                       "trapped on an encoding the reference accepted\n",
                       round, n);
                describe(insn, lens[n]);
                dump_operands(&before, lens[n] == 2 ?
                              rv_expand_c((uint16_t)insn) : insn);
                rc = 1;
                break;
            }

            if (gdb_read_state(&g, &st)) {
                fprintf(stderr, "fuzz: cannot read remote state\n");
                rc = 1;
                break;
            }

            if (cpu.pc != st.pc) {
                printf("DIVERGENCE round %ld instruction %d: pc "
                       "ours=%08x qemu=%08x\n", round, n, cpu.pc, st.pc);
                describe(insn, lens[n]);
                rc = 1;
                break;
            }
            for (i = 1; i < 32; i++) {
                if (cpu.x[i] != st.x[i]) {
                    printf("DIVERGENCE round %ld instruction %d: x%d (%s) "
                           "ours=%08x qemu=%08x\n", round, n, i,
                           rv_x_names[i], cpu.x[i], st.x[i]);
                    describe(insn, lens[n]);
                    dump_operands(&before, lens[n] == 2 ?
                                  rv_expand_c((uint16_t)insn) : insn);
                    rc = 1;
                    break;
                }
            }
            if (rc)
                break;

            {
                uint32_t eff = lens[n] == 2 ? rv_expand_c((uint16_t)insn)
                                            : insn;
                uint32_t op = eff & 0x7f;

                is_fp = op == 0x53 || op == 0x43 || op == 0x47 ||
                        op == 0x4b || op == 0x4f;
                rd = eff >> 7 & 0x1f;
            }
            full = (n % FULL_CHECK) == FULL_CHECK - 1;

            if (is_fp) {
                fp_total++;
                if (gdb_read_fcsr(&g, &fcsr_q)) {
                    fprintf(stderr, "fuzz: cannot read remote fcsr\n");
                    rc = 1;
                    break;
                }
                if ((cpu.fcsr & 0xff) != (fcsr_q & 0xff)) {
                    printf("DIVERGENCE round %ld instruction %d: fcsr "
                           "ours=%02x qemu=%02x\n", round, n,
                           cpu.fcsr & 0xff, fcsr_q & 0xff);
                    describe(insn, lens[n]);
                    dump_operands(&before, lens[n] == 2 ?
                                  rv_expand_c((uint16_t)insn) : insn);
                    printf("  our result:       x%u=%08x f%u=%08x\n",
                           rd, cpu.x[rd], rd, cpu.f[rd]);
                    rc = 1;
                    break;
                }
                /* Instructions that write an integer register have already
                 * been checked above; the rest write a float register. */
                if (gdb_read_freg(&g, (int)rd, &fq)) {
                    fprintf(stderr, "fuzz: cannot read remote f%u\n", rd);
                    rc = 1;
                    break;
                }
                if (cpu.f[rd] != (uint32_t)fq) {
                    printf("DIVERGENCE round %ld instruction %d: f%u (%s) "
                           "ours=%08x qemu=%08x\n", round, n, rd,
                           rv_f_names[rd], cpu.f[rd], (uint32_t)fq);
                    describe(insn, lens[n]);
                    dump_operands(&before, lens[n] == 2 ?
                                  rv_expand_c((uint16_t)insn) : insn);
                    rc = 1;
                    break;
                }
            }

            if (full) {
                /* Periodically verify the whole floating-point file, which
                 * catches a result written to the wrong register. */
                if (gdb_read_fpu(&g, &st)) {
                    fprintf(stderr, "fuzz: cannot read remote fpu\n");
                    rc = 1;
                    break;
                }
                for (i = 0; i < 32; i++) {
                    if (cpu.f[i] != (uint32_t)st.f[i]) {
                        printf("DIVERGENCE round %ld instruction %d: f%d "
                               "(%s) ours=%08x qemu=%08x on the periodic "
                               "sweep\n", round, n, i, rv_f_names[i],
                               cpu.f[i], (uint32_t)st.f[i]);
                        rc = 1;
                        break;
                    }
                }
                if (rc)
                    break;
            }
        }

        if (!quiet && !rc)
            printf("  round %ld: %ld instructions checked (%ld "
                   "floating-point)\n", round, total, fp_total);
    }

    printf("\n%ld instructions compared, %ld of them floating-point\n",
           total, fp_total);
    printf("%s\n", rc ? "FUZZ FAILED" : "no divergence found");

    free(block);
    gdb_close(&g);
    machine_free(&m);
    return rc;
}
