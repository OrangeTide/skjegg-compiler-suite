/* test_program.c : freestanding RV32 test program */
/* Cross-compile: riscv64-linux-gnu-gcc -march=rv32imfc -mabi=ilp32f
 *                -O2 -nostdlib -static -T link.ld */
/* Copyright (c) 2026 Jon Mayo - MIT-0 OR Public Domain */

/* The five integer and floating-point tests mirror the ColdFire V4e test
 * program from the earlier article so the two emulators can be compared on
 * identical algorithms. The ColdFire version computes the Newton iteration
 * in double precision; RV32F has only single precision, so this version
 * uses float. That difference is a property of the two instruction sets,
 * not of the test. */

typedef unsigned int uint32_t;
typedef int int32_t;

/* Result slots placed at fixed linker-assigned addresses */
volatile uint32_t result_fib    __attribute__((section(".results"))) = 0;
volatile uint32_t result_gcd    __attribute__((section(".results"))) = 0;
volatile uint32_t result_sum    __attribute__((section(".results"))) = 0;
volatile uint32_t result_bits   __attribute__((section(".results"))) = 0;
volatile uint32_t result_sqrt_i __attribute__((section(".results"))) = 0;

/****************************************************************
 * Console output through the write syscall
 ****************************************************************/

static void
sys_write(const char *buf, uint32_t len)
{
    register uint32_t a0 __asm__("a0") = 1;
    register const char *a1 __asm__("a1") = buf;
    register uint32_t a2 __asm__("a2") = len;
    register uint32_t a7 __asm__("a7") = 64;

    __asm__ volatile("ecall"
                     : "+r"(a0)
                     : "r"(a1), "r"(a2), "r"(a7)
                     : "memory");
}

static void
puts_raw(const char *s)
{
    uint32_t n = 0;

    while (s[n])
        n++;
    sys_write(s, n);
}

static void
put_uint(uint32_t v)
{
    char buf[12];
    int i = 12;

    if (v == 0) {
        sys_write("0", 1);
        return;
    }
    while (v) {
        buf[--i] = (char)('0' + v % 10);
        v /= 10;
    }
    sys_write(buf + i, (uint32_t)(12 - i));
}

static void
report(const char *name, uint32_t got, uint32_t want)
{
    puts_raw(got == want ? "  ok   " : "  FAIL ");
    puts_raw(name);
    puts_raw(" got ");
    put_uint(got);
    puts_raw(" want ");
    put_uint(want);
    puts_raw("\n");
}

/****************************************************************
 * Integer tests
 ****************************************************************/

/* Recursion, stack traffic, conditional branches */
static uint32_t
fibonacci(uint32_t n)
{
    if (n <= 1)
        return n;
    return fibonacci(n - 1) + fibonacci(n - 2);
}

/* remu from the M extension, plus a loop */
static uint32_t
gcd(uint32_t a, uint32_t b)
{
    while (b != 0) {
        uint32_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

/* Loop, add, compare */
static uint32_t
sum_to(uint32_t n)
{
    uint32_t s = 0;
    uint32_t i;

    for (i = 1; i <= n; i++)
        s += i;
    return s;
}

/* sll, srl, xor, and, or */
static uint32_t
bit_test(uint32_t x)
{
    uint32_t a = x << 4;
    uint32_t b = x >> 2;
    uint32_t c = a ^ b;
    uint32_t d = c & 0xFF00;
    uint32_t e = d | 0x0055;

    return e;
}

/* mul, mulh, div, rem across the signed and unsigned forms */
static uint32_t
muldiv_test(void)
{
    int32_t a = -1234567;
    int32_t b = 89;
    uint32_t ua = 0xDEADBEEF;
    uint32_t ub = 0x1234;
    uint32_t acc = 0;

    acc += (uint32_t)(a / b);           /* div */
    acc += (uint32_t)(a % b);           /* rem */
    acc += ua / ub;                     /* divu */
    acc += ua % ub;                     /* remu */
    acc += (uint32_t)(a * b);           /* mul */
    acc += (uint32_t)(((long long)a * b) >> 32);        /* mulh */
    acc += (uint32_t)(((unsigned long long)ua * ub) >> 32); /* mulhu */
    return acc;
}

/****************************************************************
 * Floating-point test
 ****************************************************************/

/* Newton's method for a square root, returned scaled by 1000 */
static uint32_t
sqrt_approx(float x)
{
    float guess = x / 2.0f;
    int i;

    for (i = 0; i < 20; i++)
        guess = (guess + x / guess) / 2.0f;

    return (uint32_t)(guess * 1000.0f);
}

/****************************************************************
 * Entry point
 ****************************************************************/

int
main(void)
{
    int fails = 0;

    result_fib    = fibonacci(10);
    result_gcd    = gcd(252, 105);
    result_sum    = sum_to(100);
    result_bits   = bit_test(0xAB);
    result_sqrt_i = sqrt_approx(2.0f);

    puts_raw("rv32 test program\n");
    report("fibonacci(10)", result_fib, 55);
    report("gcd(252,105) ", result_gcd, 21);
    report("sum_to(100)  ", result_sum, 5050);
    report("bit_test(0xAB)", result_bits, 0x0A55);
    report("sqrt(2)*1000 ", result_sqrt_i, 1414);
    report("muldiv       ", muldiv_test(), 4185884566u);

    fails += result_fib != 55;
    fails += result_gcd != 21;
    fails += result_sum != 5050;
    fails += result_bits != 0x0A55;
    fails += result_sqrt_i != 1414;
    fails += muldiv_test() != 4185884566u;

    puts_raw(fails ? "FAILED\n" : "all tests passed\n");
    return fails;
}
