/* zcb_guest.c : a guest that exercises the compressed byte and halfword
 * operations */
/* Cross-compile: riscv64-linux-gnu-gcc -march=rv32imafc_zba_zbb_zbs_zcb
 *                -mabi=ilp32f -O2 -nostdlib -static -T link.ld */
/* Copyright (c) 2026 Jon Mayo - MIT-0 OR Public Domain */

/* Zcb is the one extension here the fuzzer cannot cover. Five of its eleven
 * instructions are loads and stores, and the fuzzer deliberately generates
 * no memory access, because a random address would end the run. So this
 * guest carries the weight: byte and halfword traffic written the way a
 * compiler compresses it, run under lockstep.c against qemu.
 *
 * Compression happens in the assembler rather than the compiler, and it
 * only applies when the operands fit: both registers in x8 to x15, and an
 * offset of 0 to 3 for a byte or 0 or 2 for a halfword. Small structures
 * and short buffers are what produce those, so that is what this does. */

typedef unsigned int uint32_t;
typedef int int32_t;

volatile uint32_t result_bytes  __attribute__((section(".results"))) = 0;
volatile uint32_t result_halves __attribute__((section(".results"))) = 0;
volatile uint32_t result_pixels __attribute__((section(".results"))) = 0;
volatile uint32_t result_str    __attribute__((section(".results"))) = 0;
volatile uint32_t result_misc   __attribute__((section(".results"))) = 0;

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
 * Byte traffic
 ****************************************************************/

static unsigned char buf[256];

/* A four-byte record is what makes the assembler reach for c.lbu and c.sb:
 * every field is at offset 0 to 3 from one base register. */
struct packet {
    unsigned char kind;
    unsigned char flags;
    unsigned char len;
    unsigned char sum;
};

static struct packet packets[32];

static uint32_t
byte_work(void)
{
    uint32_t acc = 0, v = 0x5eed1234u;
    int i;

    for (i = 0; i < 32; i++) {
        packets[i].kind = (unsigned char)(v >> 24);
        packets[i].flags = (unsigned char)(v >> 16);
        packets[i].len = (unsigned char)(v >> 8);
        packets[i].sum = (unsigned char)(packets[i].kind ^
                                         packets[i].flags ^
                                         packets[i].len);
        v = v * 1103515245u + 12345u;
    }
    for (i = 0; i < 32; i++) {
        acc += packets[i].kind;
        acc ^= (uint32_t)packets[i].flags << 8;
        acc += packets[i].len;
        if (packets[i].sum & 1)
            acc = ~acc;
    }
    return acc;
}

/****************************************************************
 * Halfword traffic, signed and unsigned
 ****************************************************************/

struct sample {
    short left;
    short right;
};

static struct sample samples[32];

/* c.lh is the only Zcb load that sign extends, so half of this reads
 * signed and half unsigned, and the values straddle 0x8000. */
static uint32_t
half_work(void)
{
    uint32_t acc = 0, v = 0x0badf00du;
    int i;

    for (i = 0; i < 32; i++) {
        samples[i].left = (short)(v >> 16);
        samples[i].right = (short)v;
        v = v * 22695477u + 1u;
    }
    for (i = 0; i < 32; i++) {
        acc += (uint32_t)(int32_t)samples[i].left;
        acc ^= (uint32_t)(unsigned short)samples[i].right;
        acc = (acc << 3) | (acc >> 29);
    }
    return acc;
}

/****************************************************************
 * A byte buffer walked the way a codec walks one
 ****************************************************************/

static uint32_t
pixel_work(void)
{
    uint32_t acc = 0;
    int i;

    for (i = 0; i < 256; i++)
        buf[i] = (unsigned char)(i * 7 + (i >> 3));
    /* Read and write four bytes at a time from one base pointer, which is
     * the shape that compresses */
    for (i = 0; i + 3 < 256; i += 4) {
        unsigned char *p = &buf[i];
        unsigned char a = p[0], b = p[1], c = p[2], d = p[3];

        p[0] = (unsigned char)(a + d);
        p[1] = (unsigned char)(b ^ c);
        p[2] = (unsigned char)(c - a);
        p[3] = (unsigned char)(d | 1);
    }
    for (i = 0; i < 256; i++)
        acc = acc * 31u + buf[i];
    return acc;
}

/****************************************************************
 * String work, where the byte loads come from the loop itself
 ****************************************************************/

static const char *const words[] = {
    "compressed", "byte", "and", "halfword", "operations", "",
    "a", "much longer string than the others to move the pointer along",
};

static uint32_t
string_work(void)
{
    uint32_t acc = 0;
    unsigned i;

    for (i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        const char *s = words[i];
        uint32_t n = 0;

        while (*s) {
            acc = acc * 131u + (unsigned char)*s;
            n++;
            s++;
        }
        acc ^= n << 16;
    }
    return acc;
}

/****************************************************************
 * c.not and c.mul, which have no memory in them at all
 ****************************************************************/

static uint32_t
misc_work(void)
{
    uint32_t acc = 1, v = 0x1234u;
    int i;

    for (i = 0; i < 32; i++) {
        acc = acc * v;                  /* c.mul when both fit */
        acc = ~acc;                     /* c.not */
        acc += (unsigned char)i;
        v += 3;
    }
    return acc;
}

/****************************************************************
 * The forms the compiler did not reach here
 ****************************************************************/

/* c.sh, the sign-extending unary forms and c.mul only compress when both
 * registers land in x8 to x15, which the code above does not always give
 * them. Naming the registers reaches the encodings directly. The operands
 * are chosen so a wrong sign extension changes the answer. */
static uint32_t
explicit_forms(void)
{
    register uint32_t v __asm__("a0");
    register uint32_t w __asm__("a1");
    register unsigned short *p __asm__("a2") = (unsigned short *)buf;
    uint32_t acc;

    v = 0x1234f0f0u;
    w = 3;
    __asm__ volatile("c.sh %0, 0(%1)" : : "r"(v), "r"(p) : "memory");
    __asm__ volatile("c.sh %0, 2(%1)" : : "r"(v), "r"(p) : "memory");
    acc = (uint32_t)p[0] + ((uint32_t)p[1] << 8);

    v = 0x000000f0u;
    __asm__("c.sext.b %0" : "+r"(v));
    acc ^= v;

    v = 0x0000f000u;
    __asm__("c.sext.h %0" : "+r"(v));
    acc += v;

    v = 0xfeedf00du;
    __asm__("c.zext.h %0" : "+r"(v));
    acc ^= v;

    v = 0x00010003u;
    __asm__("c.mul %0, %1" : "+r"(v) : "r"(w));
    return acc + v;
}

int
main(void)
{
    int fails = 0;

    result_bytes  = byte_work();
    result_halves = half_work();
    result_pixels = pixel_work();
    result_str    = string_work();
    result_misc   = misc_work() ^ explicit_forms();

    puts_raw("rv32 compressed byte and halfword guest\n");

    /* Produced by qemu-riscv32 running this same binary */
    report("bytes  ", result_bytes, 0xffff8e5cu);
    report("halves ", result_halves, 0x21e62ce0u);
    report("pixels ", result_pixels, 0x126ab780u);
    report("strings", result_str, 0xfc28c059u);
    report("misc   ", result_misc, 0xab8ff71du);

    fails += result_bytes != 0xffff8e5cu;
    fails += result_halves != 0x21e62ce0u;
    fails += result_pixels != 0x126ab780u;
    fails += result_str != 0xfc28c059u;
    fails += result_misc != 0xab8ff71du;

    puts_raw(fails ? "FAILED\n" : "all tests passed\n");
    return fails;
}
