/* bitmanip_guest.c : a guest that exercises Zba, Zbb and Zbs */
/* Cross-compile: riscv64-linux-gnu-gcc -march=rv32imafc_zba_zbb_zbs
 *                -mabi=ilp32f -O2 -nostdlib -static -T link.ld */
/* Copyright (c) 2026 Jon Mayo - MIT-0 OR Public Domain */

/* The unit tests drive the bit-manipulation instructions from hand-built
 * encodings and the fuzzer drives them from random ones. Neither covers the
 * path this program covers: instructions a compiler chose to emit, running
 * inside real control flow, on values that come from earlier results. Run
 * it under lockstep.c to compare it against qemu instruction by
 * instruction.
 *
 * The routines are written the way a compiler recognizes. GCC turns the
 * builtins into clz, ctz and cpop, the shifted array indexing into sh1add,
 * sh2add and sh3add, and the ternary comparisons into min and max. The
 * forms it will not choose on its own are reached through inline assembly,
 * which is also how a real program would use them. */

typedef unsigned int uint32_t;
typedef int int32_t;

volatile uint32_t result_hash  __attribute__((section(".results"))) = 0;
volatile uint32_t result_pop   __attribute__((section(".results"))) = 0;
volatile uint32_t result_index __attribute__((section(".results"))) = 0;
volatile uint32_t result_bits  __attribute__((section(".results"))) = 0;
volatile uint32_t result_asm   __attribute__((section(".results"))) = 0;

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
put_hex(uint32_t v)
{
    static const char digits[] = "0123456789abcdef";
    char buf[8];
    int i;

    for (i = 7; i >= 0; i--) {
        buf[i] = digits[v & 15];
        v >>= 4;
    }
    sys_write(buf, 8);
}

static void
report(const char *name, uint32_t got, uint32_t want)
{
    puts_raw(got == want ? "  ok   " : "  FAIL ");
    puts_raw(name);
    puts_raw(" got ");
    put_hex(got);
    puts_raw(" want ");
    put_hex(want);
    puts_raw("\n");
}

/****************************************************************
 * Counting: clz, ctz and cpop
 ****************************************************************/

/* A mix that keeps every input different from the last, so that a
 * mispredicted branch inside a counting loop cannot hide a wrong answer. */
static uint32_t
counting(void)
{
    uint32_t acc = 0, v = 0x12345678u;
    int i;

    for (i = 0; i < 64; i++) {
        acc += (uint32_t)__builtin_clz(v | 1);
        acc = acc * 3 + (uint32_t)__builtin_ctz(v | 0x80000000u);
        acc ^= (uint32_t)__builtin_popcount(v);
        v = v * 1103515245u + 12345u;
    }
    return acc;
}

/****************************************************************
 * Address generation: the shifted adds
 ****************************************************************/

static uint32_t words[64];
static unsigned short halves[64];
static unsigned char bytes[64];

/* Three element widths in one loop, because each width is a different
 * shifted add and the compiler picks between them by scale. */
static uint32_t
indexing(void)
{
    uint32_t acc = 0;
    int i;

    for (i = 0; i < 64; i++) {
        words[i] = (uint32_t)(i * 2654435761u);
        halves[i] = (unsigned short)(i * 40503u);
        bytes[i] = (unsigned char)(i * 131u);
    }
    for (i = 0; i < 64; i++) {
        uint32_t j = words[i] & 63u;

        acc += words[j];
        acc ^= halves[j];
        acc += bytes[j];
        acc = (acc << 1) | (acc >> 31);
    }
    return acc;
}

/****************************************************************
 * Single-bit work and the comparisons
 ****************************************************************/

static uint32_t
bitset_work(void)
{
    uint32_t set = 0, acc = 0, v = 0xdeadbeefu;
    int i;

    for (i = 0; i < 96; i++) {
        uint32_t bit = v & 31u;

        if (v & 0x10000u)
            set |= 1u << bit;               /* bset */
        else
            set &= ~(1u << bit);            /* bclr */
        if ((set >> bit) & 1u)              /* bext */
            acc += bit;
        set ^= 1u << ((bit + 7) & 31);      /* binv */
        v = v * 1664525u + 1013904223u;
    }
    return acc ^ set;
}

static uint32_t
minmax(void)
{
    int32_t lo = 0x7fffffff, hi = (int32_t)0x80000000;
    uint32_t ulo = 0xffffffffu, uhi = 0, v = 0x0badc0deu;
    int i;

    for (i = 0; i < 64; i++) {
        int32_t s = (int32_t)v;

        lo = s < lo ? s : lo;
        hi = s > hi ? s : hi;
        ulo = v < ulo ? v : ulo;
        uhi = v > uhi ? v : uhi;
        v = v * 22695477u + 1u;
    }
    return (uint32_t)lo ^ (uint32_t)hi ^ ulo ^ uhi;
}

/****************************************************************
 * The forms a compiler will not choose on its own
 ****************************************************************/

static uint32_t
explicit_forms(uint32_t v)
{
    uint32_t rev, orc, rot, an, xn, sx;

    __asm__("rev8  %0, %1" : "=r"(rev) : "r"(v));
    __asm__("orc.b %0, %1" : "=r"(orc) : "r"(v));
    __asm__("rori  %0, %1, 13" : "=r"(rot) : "r"(v));
    __asm__("andn  %0, %1, %2" : "=r"(an) : "r"(v), "r"(rev));
    __asm__("xnor  %0, %1, %2" : "=r"(xn) : "r"(v), "r"(orc));
    __asm__("sext.b %0, %1" : "=r"(sx) : "r"(v));

    return rev ^ orc ^ rot ^ an ^ xn ^ sx;
}

int
main(void)
{
    int fails = 0;

    result_pop   = counting();
    result_index = indexing();
    result_bits  = bitset_work();
    result_hash  = minmax();
    result_asm   = explicit_forms(0x0f00ba5eu) ^
                   explicit_forms(0x80000001u) ^
                   explicit_forms(0x00000000u);

    puts_raw("rv32 bit manipulation guest\n");

    /* The expected values were produced by qemu-riscv32 running this same
     * binary, so a disagreement means the emulator and the reference
     * differ rather than that the program was rewritten. */
    report("counting  ", result_pop, 0xd103453fu);
    report("indexing  ", result_index, 0xb67973ffu);
    report("bitset    ", result_bits, 0xd470dbf9u);
    report("minmax    ", result_hash, 0x037c9001u);
    report("explicit  ", result_asm, 0x7cb98724u);

    fails += result_pop != 0xd103453fu;
    fails += result_index != 0xb67973ffu;
    fails += result_bits != 0xd470dbf9u;
    fails += result_hash != 0x037c9001u;
    fails += result_asm != 0x7cb98724u;

    puts_raw(fails ? "FAILED\n" : "all tests passed\n");
    return fails;
}
