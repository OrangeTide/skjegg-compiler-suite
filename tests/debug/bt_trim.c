/* bt_trim.c : host unit test for dw_backtrace_format's corrupt-chain trim.  The
   fp walk (dw_backtrace) is bounded against a corrupt chain, but a monotonic fp
   can still carry a garbage return address.  The formatter looks each frame's
   address up in the function table, and a caller frame (not the live pc) that
   lands in no known function ends the output with one truncation marker instead
   of printing bogus frames to the depth cap.

   The reader is built by hand (two known functions, no line table), so the test
   needs no ELF, no DWARF, and no qemu: it exercises the formatter alone.  Pairs
   with tests/debug/bt_loop.c (the walk's termination guards). */

#include "dwread.h"

#include <stdio.h>
#include <string.h>

/* Two adjacent known functions: main at [0x1000, 0x1100), wrap at
   [0x1100, 0x1200).  Any address outside both is "no known function". */
static struct dw_func g_funcs[] = {
    { 0x1000, 0x100, "main" },
    { 0x1100, 0x100, "wrap" },
};

static struct dw_reader g_reader = {
    NULL, 0,            /* lines, nlines (none: no "at file:line" clause) */
    NULL, 0,            /* files, nfiles */
    NULL, NULL,         /* comp_dir, cu_name */
    g_funcs, 2,         /* funcs, nfuncs (sorted by addr ascending) */
    NULL,               /* arena */
};

static int fails;

static void
expect(const char *what, int cond)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        fails++;
    }
}

int
main(void)
{
    char buf[2048];

    /* A chain that wanders: main -> wrap -> garbage.  The garbage caller frame
       is trimmed to one marker; no third real frame is printed. */
    {
        uint32_t pcs[3] = { 0x1050, 0x1180, 0x0000dead };
        dw_backtrace_format(&g_reader, pcs, 3, buf, sizeof buf);
        expect("known frames are symbolized", strstr(buf, "main") != NULL
               && strstr(buf, "wrap") != NULL);
        expect("the garbage caller is marked truncated",
               strstr(buf, "truncated") != NULL);
        expect("the garbage address is shown once",
               strstr(buf, "0x0000dead") != NULL);
        expect("the walk stops at the garbage frame (#2 is the last line)",
               strstr(buf, "#2") != NULL && strstr(buf, "#3") == NULL);
    }

    /* The live pc (frame 0) may itself be a wild address; it is the fault site,
       so it is printed as ?? and never triggers the trim. */
    {
        uint32_t pcs[2] = { 0x0000dead, 0x1050 };
        dw_backtrace_format(&g_reader, pcs, 2, buf, sizeof buf);
        expect("a wild live pc is printed, not trimmed",
               strstr(buf, "0x0000dead") != NULL
               && strstr(buf, "??") != NULL);
        expect("the real caller below a wild pc is still symbolized",
               strstr(buf, "main") != NULL);
        expect("a wild frame 0 does not truncate the trace",
               strstr(buf, "truncated") == NULL);
    }

    /* With no reader every frame is unknown, so raw addresses are all there is:
       no trim (otherwise a symbol-less child would print only its live pc). */
    {
        uint32_t pcs[3] = { 0x1050, 0x0000dead, 0x0000beef };
        dw_backtrace_format(NULL, pcs, 3, buf, sizeof buf);
        expect("without a reader all frames print as raw addresses",
               strstr(buf, "#0") != NULL && strstr(buf, "#1") != NULL
               && strstr(buf, "#2") != NULL);
        expect("without a reader nothing is marked truncated",
               strstr(buf, "truncated") == NULL);
    }

    /* A fully known chain is untouched: no marker. */
    {
        uint32_t pcs[2] = { 0x1050, 0x1180 };
        dw_backtrace_format(&g_reader, pcs, 2, buf, sizeof buf);
        expect("a clean chain is not trimmed",
               strstr(buf, "truncated") == NULL
               && strstr(buf, "main") != NULL && strstr(buf, "wrap") != NULL);
    }

    if (fails == 0)
        printf("PASS: dw_backtrace_format trims a chain that wanders into "
               "unknown code\n");
    return fails ? 1 : 0;
}
