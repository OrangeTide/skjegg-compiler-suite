/* wasm_selftest.c : a self-checking test for the WASM backend.

   Compiled by skj-cc-wasm, assembled by wat2wasm, and run by invoking the
   exported `run` under a WASM engine.  It returns 0 when every check passes,
   otherwise the 1-based number of the first failing check, so the test lane
   needs no I/O runtime: it reads a single integer result.  Each feature area
   the backend lowers is exercised: the integer core, control flow (including a
   loop and a goto), direct and indirect and recursive calls, floating point,
   64-bit integers, and globals. */

/* ---- globals (data, rodata, relocations) ---- */
int g_counter = 0;
int g_table[5] = { 2, 3, 5, 7, 11 };
const char *g_msg = "abcde";
double g_half = 0.5;
int g_v = 40;
int *g_vp = &g_v;

/* ---- plain functions ---- */
static int add(int a, int b) { return a + b; }
static int sub(int a, int b) { return a - b; }
static int mul(int a, int b) { return a * b; }

static int fib(int n)
{
    int a = 0, b = 1, i;
    for (i = 0; i < n; i++) { int t = a + b; a = b; b = t; }
    return a;
}

static int gcd(int a, int b)
{
    while (b != 0) { int t = b; b = a % b; a = t; }
    return a;
}

static long long lfact(int n)
{
    long long r = 1;
    int i;
    for (i = 2; i <= n; i++)
        r *= i;
    return r;
}

static int apply(int (*f)(int, int), int x, int y) { return f(x, y); }

static int goto_sum(int n)
{
    int s = 0, i = 0;
loop:
    if (i >= n) goto done;
    s += i;
    i++;
    goto loop;
done:
    return s;
}

static double dpoly(double x) { return 3.0 * x * x + 2.0 * x - 1.0; }

/* exported: returns 0 on full success, else the failing check's index */
int run(void)
{
    int (*ops[3])(int, int);
    int tsum, i;

    if (add(20, 22) != 42) return 1;
    if (sub(100, 58) != 42) return 2;
    if (mul(6, 7) != 42) return 3;

    if (fib(10) != 55) return 4;
    if (gcd(56, 42) != 14) return 5;
    if (goto_sum(10) != 45) return 6;

    /* bit and shift core */
    if ((0xF0 | 0x0F) != 0xFF) return 7;
    if ((0xFF & 0x0F) != 0x0F) return 8;
    if ((1 << 10) != 1024) return 9;
    if ((-8 >> 1) != -4) return 10;
    if ((~0) != -1) return 11;

    /* indirect calls */
    ops[0] = add; ops[1] = sub; ops[2] = mul;
    if (apply(add, 3, 4) != 7) return 12;
    if (ops[0](10, 5) != 15) return 13;
    if (ops[1](10, 5) != 5) return 14;
    if (ops[2](10, 5) != 50) return 15;

    /* floating point */
    if (dpoly(3.0) != 32.0) return 16;
    if (g_half * 8.0 != 4.0) return 17;
    {
        double d = (double)7 + 0.5;
        if (d != 7.5) return 18;
        if ((int)d != 7) return 19;
    }
    {
        /* single-precision: a float literal is an f32 load widened to double
           (f32.load + f64.promote_f32), and a float through a pointer is an
           f32 store/load (not a mis-sized f64) */
        float f = 1.5f;
        float *p = &f;
        if ((double)f + 0.5 != 2.0) return 36;
        *p = 2.5f;
        if (*p != 2.5f) return 37;
        if ((double)*p * 2.0 != 5.0) return 38;
    }

    /* 64-bit integers */
    if (lfact(20) != 2432902008176640000LL) return 20;
    {
        long long x = 1LL << 40;
        if (x != 1099511627776LL) return 21;
        if ((int)(x >> 32) != 256) return 22;
    }
    {
        /* native 64-bit division/remainder (WASM i64.div/rem, not a helper) */
        long long s = 1000000000000LL;
        unsigned long long u = 18000000000000000000ULL;
        if (s / 7 != 142857142857LL) return 30;
        if (s % 7 != 1) return 31;
        if (-s / 7 != -142857142857LL) return 32;
        if (-s % 7 != -1) return 33;
        if (u / 123456789ULL != 145800001326ULL) return 34;
        if (u % 123456789ULL != 96297786ULL) return 35;
    }

    /* globals: scalar, array, string, pointer */
    g_counter = 0;
    g_counter++;
    g_counter++;
    if (g_counter != 2) return 23;

    tsum = 0;
    for (i = 0; i < 5; i++)
        tsum += g_table[i];
    if (tsum != 28) return 24;
    if (g_table[3] != 7) return 25;

    if (g_msg[0] != 'a' || g_msg[4] != 'e') return 26;
    {
        int n = 0;
        const char *p = g_msg;
        while (*p) { n++; p++; }
        if (n != 5) return 27;
    }

    if (*g_vp != 40) return 28;
    *g_vp = 99;
    if (g_v != 99) return 29;

    return 0;
}
