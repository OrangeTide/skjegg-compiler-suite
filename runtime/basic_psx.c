/* basic_psx.c : the PlayStation seam for the BASIC runtime.
 *
 * basic_rt.c is written against a small set of syscall-style seam calls. On a
 * hosted target (qemu, skj-run) those are Linux syscalls reached through the
 * crt. The PlayStation has no syscalls: write, exit and the bump arena come
 * from the BIOS-TTY crt (start_psx.S), and the four calls below map to the
 * console's own facilities.
 *
 * read has no console stdin, so it reports end of file. The clock, the sleep
 * and the random source all derive from root counter 2, the system clock
 * divided by 8 (about 4.2336 MHz). That is the one free-running hardware timer
 * a text-only program can use without bringing up the GPU. The counter is 16
 * bits and wraps about every 15.5 ms, so rcnt2_now accumulates a wrap-corrected
 * 64-bit tick count and must be polled more often than one full period, which
 * the sleep loop and the per-statement clock reads both satisfy.
 *
 * This file is compiled by skj-cc-mips-sf, like basic_rt.c, so the MMIO loads
 * and stores keep the soft-float ABI the rest of the runtime uses. */

#define RCNT2_COUNT  (*(volatile unsigned short *)0x1F801120)
#define RCNT2_MODE   (*(volatile unsigned short *)0x1F801124)
#define RCNT2_TARGET (*(volatile unsigned short *)0x1F801128)

#define RCNT2_HZ 4233600u               /* 33.8688 MHz / 8 */

static int rcnt2_inited;
static unsigned short rcnt2_last;
static unsigned long long rcnt2_ticks;  /* monotonic, wrap-corrected */

static void
rcnt2_init(void)
{
    /* Counter 2: clock source = system clock / 8 (mode bits 9:8 = 0b10), free
       running (no synchronisation), wrap at 0xFFFF, no interrupt. */
    RCNT2_MODE = 0x0200;
    RCNT2_TARGET = 0xFFFF;
    RCNT2_COUNT = 0;
    rcnt2_last = 0;
    rcnt2_ticks = 0;
    rcnt2_inited = 1;
}

static unsigned long long
rcnt2_now(void)
{
    unsigned short c;

    if (!rcnt2_inited)
        rcnt2_init();
    c = RCNT2_COUNT;
    rcnt2_ticks += (unsigned short)(c - rcnt2_last);
    rcnt2_last = c;
    return rcnt2_ticks;
}

struct psx_timespec {
    int sec;
    int nsec;
};

/* clock_gettime(clk_id, struct timespec *ts): a 32-bit {tv_sec, tv_nsec} from
   the monotonic tick count. The clock id is ignored; there is one clock. */
int
clock_gettime(int clk_id, void *tsp)
{
    struct psx_timespec *ts = tsp;
    unsigned long long t = rcnt2_now();
    unsigned sec = (unsigned)(t / RCNT2_HZ);
    unsigned rem = (unsigned)(t % RCNT2_HZ);

    (void)clk_id;
    if (ts) {
        ts->sec = (int)sec;
        /* rem ticks -> ns, in 64-bit: rem < RCNT2_HZ, so rem * 1e9 fits. */
        ts->nsec = (int)((unsigned long long)rem * 1000000000ull / RCNT2_HZ);
    }
    return 0;
}

/* nanosleep(req, rem): busy-wait on the tick count for the requested interval.
   rem is not written; the console delivers no signals, so the sleep always
   completes. */
int
nanosleep(const void *reqp, void *remp)
{
    const struct psx_timespec *req = reqp;
    unsigned long long ns, end;

    (void)remp;
    if (!req)
        return -1;
    ns = (unsigned long long)(unsigned)req->sec * 1000000000ull
       + (unsigned long long)(unsigned)req->nsec;
    /* ns -> ticks: ns * RCNT2_HZ / 1e9 (the product stays within 64 bits for
       any interval a program would wait). */
    end = rcnt2_now() + ns * RCNT2_HZ / 1000000000ull;
    while (rcnt2_now() < end)
        ;
    return 0;
}

/* getrandom(buf, len, flags): seed bytes for the RND generator. The console has
   no entropy source, so mix the timer through an LCG; it varies run to run
   because the counter value when a program first reads it does. */
int
getrandom(void *buf, int len, unsigned flags)
{
    unsigned char *p = buf;
    unsigned long long t = rcnt2_now();
    int i;

    (void)flags;
    for (i = 0; i < len; i++) {
        t = t * 6364136223846793005ull + 1442695040888963407ull;
        p[i] = (unsigned char)(t >> 33);
    }
    return len;
}

/* read(fd, buf, len): the console TTY is output only, so report EOF. INPUT in a
   PlayStation BASIC program reads nothing. */
int
read(int fd, void *buf, int len)
{
    (void)fd;
    (void)buf;
    (void)len;
    return 0;
}
