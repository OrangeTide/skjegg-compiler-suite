/* f32_oracle.c : generate the f32 differential golden master.
 *
 * The oracle is the RV32 emulator's own single-precision core (emu/rv32.c),
 * which is already verified bit-for-bit against qemu-riscv32 by the fuzz,
 * lockstep and riscv-arch-test rigs, and is what smolmoo runs.  For each
 * input vector in f32_vectors.h this assembles the one RV instruction that
 * performs the operation, runs it through the real decoder + f32 core (the
 * same driving pattern as tests/test_rv_fp.c), and records the result.
 *
 * Output is f32_golden.inc, a committed C array the differential test
 * includes.  Regenerate with `make f32-oracle`; a stale golden is caught by
 * `make f32-golden-check`.  Also writes build/f32_index.txt, a plain-text
 * numbered listing so a nonzero exit from the differential test names the
 * diverging vector.
 *
 * The op set is F-only (RV32IMAFC has F, not D), matching f32_vectors.h.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "rv32.h"

#include "f32_vectors.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEM_SIZE   0x4000
#define CODE_BASE  0x1000

static uint8_t mem[MEM_SIZE];

static uint32_t r8(void *c, uint32_t a)  { (void)c; return a < MEM_SIZE ? mem[a] : 0; }
static uint32_t r16(void *c, uint32_t a) {
    (void)c;
    if (a + 1 >= MEM_SIZE) return 0;
    return (uint32_t)mem[a] | ((uint32_t)mem[a + 1] << 8);
}
static uint32_t r32(void *c, uint32_t a) {
    (void)c;
    if (a + 3 >= MEM_SIZE) return 0;
    return (uint32_t)mem[a] | ((uint32_t)mem[a + 1] << 8) |
           ((uint32_t)mem[a + 2] << 16) | ((uint32_t)mem[a + 3] << 24);
}
static void w8(void *c, uint32_t a, uint32_t v)  { (void)c; if (a < MEM_SIZE) mem[a] = (uint8_t)v; }
static void w16(void *c, uint32_t a, uint32_t v) {
    (void)c;
    if (a + 1 < MEM_SIZE) { mem[a] = (uint8_t)v; mem[a + 1] = (uint8_t)(v >> 8); }
}
static void w32(void *c, uint32_t a, uint32_t v) {
    (void)c;
    if (a + 3 < MEM_SIZE) {
        mem[a] = (uint8_t)v; mem[a + 1] = (uint8_t)(v >> 8);
        mem[a + 2] = (uint8_t)(v >> 16); mem[a + 3] = (uint8_t)(v >> 24);
    }
}

/* R-type FP encoding: funct7[31:25] rs2[24:20] rs1[19:15] funct3[14:12]
 * rd[11:7] opcode[6:0].  funct3 is the rounding mode for arithmetic, the
 * compare selector for feq/flt/fle, and 0 for the conversions used here. */
static uint32_t
rtype(unsigned f7, unsigned rs2, unsigned rs1, unsigned f3, unsigned rd)
{
    return (f7 << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | 0x53u;
}

/* Registers used: f1/f2 float inputs, x1 int input, f3 float result, x3
 * int result.  Rounding mode is static RNE (funct3 = 0), which is what the
 * runtime default frm is on every backend, so this holds frm out of the
 * comparison. */
#define RS1_F   1
#define RS2_F   2
#define RD_F    3
#define RS1_X   1
#define RD_X    3

/* Run one vector through the core, returning its 32-bit result (float bits
 * or int, per the op). */
static uint32_t
oracle(const struct f32_vec *v)
{
    rv_cpu cpu;
    uint32_t insn;
    int result_is_int = F32_RESULT_IS_INT(v->op);

    switch (v->op) {
    case OP_ADD: insn = rtype(0x00, RS2_F, RS1_F, 0, RD_F); break;
    case OP_SUB: insn = rtype(0x04, RS2_F, RS1_F, 0, RD_F); break;
    case OP_MUL: insn = rtype(0x08, RS2_F, RS1_F, 0, RD_F); break;
    case OP_DIV: insn = rtype(0x0c, RS2_F, RS1_F, 0, RD_F); break;
    /* fneg.s = fsgnjn.s fd, fs, fs (funct7 0x10, funct3 001) */
    case OP_NEG: insn = rtype(0x10, RS1_F, RS1_F, 1, RD_F); break;
    /* feq/flt/fle.s (funct7 0x50), funct3 2/1/0, int rd */
    case OP_EQ:  insn = rtype(0x50, RS2_F, RS1_F, 2, RD_X); break;
    case OP_LT:  insn = rtype(0x50, RS2_F, RS1_F, 1, RD_X); break;
    case OP_LE:  insn = rtype(0x50, RS2_F, RS1_F, 0, RD_X); break;
    /* fcvt.s.w fd, xs (funct7 0x68, rs2 0), int in x1 */
    case OP_I2F: insn = rtype(0x68, 0, RS1_X, 0, RD_F); break;
    /* fcvt.w.s xd, fs, rtz (funct7 0x60, rs2 0, funct3 1): C's (int)float
     * truncates toward zero, which is what skj-cc emits, so the oracle
     * rounds the same way. */
    case OP_F2I: insn = rtype(0x60, 0, RS1_F, 1, RD_X); break;
    default: fprintf(stderr, "oracle: unknown op %d\n", v->op); exit(2);
    }

    memset(mem, 0, sizeof mem);
    w32(NULL, CODE_BASE, insn);

    rv_init(&cpu, r8, r16, r32, w8, w16, w32, NULL);
    rv_reset(&cpu, CODE_BASE);

    if (v->op == OP_I2F)
        cpu.x[RS1_X] = v->a;              /* signed int32 input */
    else {
        cpu.f[RS1_F] = v->a;              /* float bits */
        cpu.f[RS2_F] = v->b;
    }

    if (rv_step(&cpu) < 0) {
        fprintf(stderr, "oracle: step faulted on vector '%s'\n", v->note);
        exit(2);
    }

    return result_is_int ? cpu.x[RD_X] : cpu.f[RD_F];
}

int
main(int argc, char **argv)
{
    const char *incpath = "tests/f32_golden.inc";
    const char *idxpath = "build/f32_index.txt";
    FILE *inc, *idx;
    int i;

    if (argc > 1) incpath = argv[1];
    if (argc > 2) idxpath = argv[2];

    inc = fopen(incpath, "w");
    if (!inc) { perror(incpath); return 2; }
    idx = fopen(idxpath, "w");
    if (!idx) { perror(idxpath); return 2; }

    /* Flat, skj-cc-friendly output: no structs, no string literals, just
     * parallel arrays of primitives, so the differential test compiles on
     * every backend's C front end. */
    fprintf(inc,
        "/* f32_golden.inc : GENERATED by tests/f32_oracle.c, do not edit.\n"
        " * Golden single-precision results from the RV32 emulator's f32\n"
        " * core (emu/rv32.c), the deterministic oracle.  Regenerate with\n"
        " * `make f32-oracle`.  Made by a machine. PUBLIC DOMAIN (CC0-1.0) */\n\n"
        "#define F32_OP_ADD %d\n#define F32_OP_SUB %d\n#define F32_OP_MUL %d\n"
        "#define F32_OP_DIV %d\n#define F32_OP_NEG %d\n#define F32_OP_EQ %d\n"
        "#define F32_OP_LT %d\n#define F32_OP_LE %d\n#define F32_OP_I2F %d\n"
        "#define F32_OP_F2I %d\n\n",
        OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_NEG,
        OP_EQ, OP_LT, OP_LE, OP_I2F, OP_F2I);

    fprintf(inc, "#define F32_N %d\n\n", F32_NVEC);

    fprintf(inc, "static const unsigned char f32_op[F32_N] = {\n   ");
    for (i = 0; i < F32_NVEC; i++)
        fprintf(inc, " %u,", f32_vectors[i].op);
    fprintf(inc, "\n};\n\n");

    fprintf(inc, "static const unsigned int f32_a[F32_N] = {\n");
    for (i = 0; i < F32_NVEC; i++)
        fprintf(inc, "    0x%08xu,\n", f32_vectors[i].a);
    fprintf(inc, "};\n\n");

    fprintf(inc, "static const unsigned int f32_b[F32_N] = {\n");
    for (i = 0; i < F32_NVEC; i++)
        fprintf(inc, "    0x%08xu,\n", f32_vectors[i].b);
    fprintf(inc, "};\n\n");

    fprintf(inc, "static const unsigned int f32_exp[F32_N] = {\n");
    for (i = 0; i < F32_NVEC; i++) {
        uint32_t g = oracle(&f32_vectors[i]);
        fprintf(inc, "    0x%08xu, /* [%d] %s */\n", g, i, f32_vectors[i].note);
        fprintf(idx, "%d\t%s\t= 0x%08x\n", i, f32_vectors[i].note, g);
    }
    fprintf(inc, "};\n");
    fclose(inc);
    fclose(idx);
    printf("f32_oracle: wrote %d golden results to %s\n", F32_NVEC, incpath);
    return 0;
}
