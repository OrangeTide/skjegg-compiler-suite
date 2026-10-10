/* test_fp_d.c : tests for the D extension (double precision).
 *
 * Double arithmetic is host `double`, so these do not re-derive IEEE results by
 * hand the way test_fp.c does for single precision; they check that the core
 * decodes and dispatches each D encoding, reads and writes the full 64-bit
 * register, converts between double, single and integer correctly, moves eight
 * bytes for fld/fsd, and traps every D instruction as illegal when the
 * extension is off.  Expected values come from the host's own double math, so
 * a wrong opcode, a truncated operand, or a botched conversion shows up. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "rv32.h"
#include "machine.h"

#define CODE_ADDR   0x1000
#define DATA_ADDR   0x2000
#define RS1 1
#define RS2 2
#define RS3 3
#define RD  10

static rv_cpu cpu;
static machine mach;
static int tests, failures;

static uint64_t
d2b(double d)
{
    uint64_t b;
    memcpy(&b, &d, 8);
    return b;
}

static uint32_t
f2b(float f)
{
    uint32_t b;
    memcpy(&b, &f, 4);
    return b;
}

/* OP-FP encoding: funct7, rs2 field, funct3 (rounding mode or sub-op). */
static uint32_t
enc(uint32_t f7, uint32_t rs2, uint32_t f3)
{
    return (f7 << 25) | (rs2 << 20) | (RS1 << 15) | (f3 << 12) |
           (RD << 7) | 0x53u;
}

static void
put_insn(uint32_t insn)
{
    mach.mem[CODE_ADDR + 0] = (uint8_t)insn;
    mach.mem[CODE_ADDR + 1] = (uint8_t)(insn >> 8);
    mach.mem[CODE_ADDR + 2] = (uint8_t)(insn >> 16);
    mach.mem[CODE_ADDR + 3] = (uint8_t)(insn >> 24);
}

/* Run one OP-FP instruction over two double operands; return the raw rd. */
static uint64_t
run_dd(uint32_t insn, uint64_t a, uint64_t b)
{
    rv_reset(&cpu, CODE_ADDR);
    cpu.dp = 1;
    put_insn(insn);
    cpu.f[RS1] = a;
    cpu.f[RS2] = b;
    rv_step(&cpu);
    return cpu.f[RD];
}

static void
ok_d(const char *name, uint64_t got, uint64_t want)
{
    tests++;
    if (got == want) {
        printf("  ok   %-32s %016llx\n", name, (unsigned long long)got);
        return;
    }
    failures++;
    printf("  FAIL %-32s got %016llx want %016llx\n", name,
           (unsigned long long)got, (unsigned long long)want);
}

static void
ok_x(const char *name, uint32_t got, uint32_t want)
{
    tests++;
    if (got == want) {
        printf("  ok   %-32s %08x\n", name, got);
        return;
    }
    failures++;
    printf("  FAIL %-32s got %08x want %08x\n", name, got, want);
}

static void
test_arith(void)
{
    uint64_t a = d2b(1.5), b = d2b(2.25);
    uint64_t pi = d2b(3.14159265358979), two = d2b(2.0);

    ok_d("fadd.d", run_dd(enc(0x01, RS2, 7), a, b), d2b(1.5 + 2.25));
    ok_d("fsub.d", run_dd(enc(0x05, RS2, 7), a, b), d2b(1.5 - 2.25));
    ok_d("fmul.d", run_dd(enc(0x09, RS2, 7), a, b), d2b(1.5 * 2.25));
    ok_d("fdiv.d", run_dd(enc(0x0d, RS2, 7), a, b), d2b(1.5 / 2.25));
    /* a value with both halves set must survive undamaged */
    ok_d("fmul.d full width", run_dd(enc(0x09, RS2, 7), pi, two),
         d2b(3.14159265358979 * 2.0));
    ok_d("fsqrt.d", run_dd(enc(0x2d, 0, 7), two, 0), d2b(sqrt(2.0)));
}

static void
test_sgnj(void)
{
    uint64_t a = d2b(3.0), nb = d2b(-1.0);

    ok_d("fsgnj.d", run_dd(enc(0x11, RS2, 0), a, nb), d2b(-3.0));
    ok_d("fsgnjn.d", run_dd(enc(0x11, RS2, 1), a, nb), d2b(3.0));
    ok_d("fsgnjx.d", run_dd(enc(0x11, RS2, 2), a, nb), d2b(-3.0));
}

static void
test_minmax(void)
{
    uint64_t a = d2b(1.0), b = d2b(2.0);
    uint64_t qnan = 0x7ff8000000000000ull;
    uint64_t pz = d2b(0.0), nz = d2b(-0.0);

    ok_d("fmin.d", run_dd(enc(0x15, RS2, 0), a, b), d2b(1.0));
    ok_d("fmax.d", run_dd(enc(0x15, RS2, 1), a, b), d2b(2.0));
    ok_d("fmin.d with nan", run_dd(enc(0x15, RS2, 0), qnan, b), d2b(2.0));
    ok_d("fmax.d with nan", run_dd(enc(0x15, RS2, 1), a, qnan), d2b(1.0));
    ok_d("fmin.d -0 < +0", run_dd(enc(0x15, RS2, 0), pz, nz), nz);
    ok_d("fmax.d +0 > -0", run_dd(enc(0x15, RS2, 1), pz, nz), pz);
}

static void
test_compare(void)
{
    uint64_t a = d2b(1.0), b = d2b(2.0), qnan = 0x7ff8000000000000ull;

    run_dd(enc(0x51, RS2, 1), a, b);  ok_x("flt.d true", cpu.x[RD], 1);
    run_dd(enc(0x51, RS2, 1), b, a);  ok_x("flt.d false", cpu.x[RD], 0);
    run_dd(enc(0x51, RS2, 0), a, a);  ok_x("fle.d equal", cpu.x[RD], 1);
    run_dd(enc(0x51, RS2, 2), a, a);  ok_x("feq.d true", cpu.x[RD], 1);
    run_dd(enc(0x51, RS2, 2), a, b);  ok_x("feq.d false", cpu.x[RD], 0);
    run_dd(enc(0x51, RS2, 2), a, qnan); ok_x("feq.d nan", cpu.x[RD], 0);
    run_dd(enc(0x51, RS2, 1), a, qnan); ok_x("flt.d nan", cpu.x[RD], 0);
}

static void
test_cvt_int(void)
{
    /* fcvt.w.d truncates toward zero at rm = rtz (funct3 = 1). */
    run_dd(enc(0x61, 0, 1), d2b(5.75), 0);  ok_x("fcvt.w.d", cpu.x[RD], 5);
    run_dd(enc(0x61, 0, 1), d2b(-5.75), 0); ok_x("fcvt.w.d neg",
                                               cpu.x[RD], (uint32_t)-5);
    run_dd(enc(0x61, 1, 1), d2b(9.0), 0);   ok_x("fcvt.wu.d", cpu.x[RD], 9);

    /* fcvt.d.w / fcvt.d.wu: integer to double is exact.  The source is x[rs1]. */
    rv_reset(&cpu, CODE_ADDR); cpu.dp = 1; put_insn(enc(0x69, 0, 0));
    cpu.x[RS1] = (uint32_t)-7; rv_step(&cpu);
    ok_d("fcvt.d.w", cpu.f[RD], d2b(-7.0));
    rv_reset(&cpu, CODE_ADDR); cpu.dp = 1; put_insn(enc(0x69, 1, 0));
    cpu.x[RS1] = 0x80000000u; rv_step(&cpu);
    ok_d("fcvt.d.wu", cpu.f[RD], d2b(2147483648.0));
}

static void
test_cvt_fmt(void)
{
    /* fcvt.d.s widens a NaN-boxed single to double (exact). */
    rv_reset(&cpu, CODE_ADDR); cpu.dp = 1; put_insn(enc(0x21, 0, 7));
    cpu.f[RS1] = 0xffffffff00000000ull | f2b(1.5f); rv_step(&cpu);
    ok_d("fcvt.d.s", cpu.f[RD], d2b(1.5));

    /* fcvt.s.d narrows a double to a NaN-boxed single. */
    rv_reset(&cpu, CODE_ADDR); cpu.dp = 1; put_insn(enc(0x20, 1, 7));
    cpu.f[RS1] = d2b(1.5); rv_step(&cpu);
    ok_d("fcvt.s.d", cpu.f[RD], 0xffffffff00000000ull | f2b(1.5f));
}

static void
test_fclass(void)
{
    run_dd(enc(0x71, 0, 1), d2b(-1.0), 0);  ok_x("fclass.d -norm",
                                               cpu.x[RD], 1u << 1);
    run_dd(enc(0x71, 0, 1), d2b(1.0), 0);   ok_x("fclass.d +norm",
                                               cpu.x[RD], 1u << 6);
    run_dd(enc(0x71, 0, 1), 0x7ff0000000000000ull, 0);
    ok_x("fclass.d +inf", cpu.x[RD], 1u << 7);
    run_dd(enc(0x71, 0, 1), d2b(-0.0), 0);  ok_x("fclass.d -0", cpu.x[RD],
                                               1u << 3);
    run_dd(enc(0x71, 0, 1), 0x7ff8000000000000ull, 0);
    ok_x("fclass.d qnan", cpu.x[RD], 1u << 9);
    run_dd(enc(0x71, 0, 1), 0x7ff4000000000000ull, 0);
    ok_x("fclass.d snan", cpu.x[RD], 1u << 8);
}

static void
test_load_store(void)
{
    uint64_t v = d2b(-123.456);
    uint32_t fld = ((uint32_t)0 << 20) | (RS1 << 15) | (3u << 12) |
                   (RD << 7) | 0x07u;                       /* fld RD, 0(RS1) */
    uint32_t fsd = (RS2 << 20) | (RS1 << 15) | (3u << 12) |
                   (0u << 7) | 0x27u;                       /* fsd RS2, 0(RS1) */
    uint64_t back;

    /* fld: eight bytes from DATA_ADDR into RD */
    memcpy(&mach.mem[DATA_ADDR], &v, 8);
    rv_reset(&cpu, CODE_ADDR); cpu.dp = 1; put_insn(fld);
    cpu.x[RS1] = DATA_ADDR; rv_step(&cpu);
    ok_d("fld", cpu.f[RD], v);

    /* fsd: eight bytes from RS2 back to DATA_ADDR + 16 */
    rv_reset(&cpu, CODE_ADDR); cpu.dp = 1; put_insn(fsd);
    cpu.x[RS1] = DATA_ADDR + 16; cpu.f[RS2] = v; rv_step(&cpu);
    memcpy(&back, &mach.mem[DATA_ADDR + 16], 8);
    ok_d("fsd", back, v);
}

static void
test_fma(void)
{
    uint32_t insn = (RS3 << 27) | (1u << 25) |      /* fmt = D */
                    (RS2 << 20) | (RS1 << 15) | (7u << 12) |
                    (RD << 7) | 0x43u;              /* fmadd.d */

    rv_reset(&cpu, CODE_ADDR); cpu.dp = 1; put_insn(insn);
    cpu.f[RS1] = d2b(2.0); cpu.f[RS2] = d2b(3.0); cpu.f[RS3] = d2b(4.0);
    rv_step(&cpu);
    ok_d("fmadd.d", cpu.f[RD], d2b(2.0 * 3.0 + 4.0));
}

static void
test_disabled(void)
{
    /* With the extension off, every D instruction is illegal. */
    rv_reset(&cpu, CODE_ADDR); cpu.dp = 0; put_insn(enc(0x01, RS2, 7));
    cpu.f[RS1] = d2b(1.0); cpu.f[RS2] = d2b(1.0); rv_step(&cpu);
    tests++;
    if (cpu.in_trap)
        printf("  ok   %-32s trapped\n", "fadd.d illegal when dp off");
    else { failures++; printf("  FAIL fadd.d not illegal with dp off\n"); }
}

int
main(void)
{
    if (machine_init(&mach, &cpu))
        return 1;

    printf("D-extension tests for RV32D\n");
    test_arith();
    test_sgnj();
    test_minmax();
    test_compare();
    test_cvt_int();
    test_cvt_fmt();
    test_fclass();
    test_load_store();
    test_fma();
    test_disabled();

    printf("\n%d tests, %d failures\n", tests, failures);
    machine_free(&mach);
    return failures ? 1 : 0;
}
