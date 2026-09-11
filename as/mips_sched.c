/* mips_sched.c : the .set reorder delay-slot scheduler
 *
 * Under `.set reorder` (the default) the assembler fills the branch and load
 * delay slots.  This source pre-pass does that, so backend output that leans
 * on the assembler assembles correctly.  A `.set noreorder` region is passed
 * through untouched, since its slots are already filled by whoever wrote it
 * (the backend's own scheduler, or hand-written systems code).
 *
 * The algorithm is the one the MIPS backend's noreorder scheduler uses, ported
 * to run on the assembler's source lines: it produces correct, runnable code
 * (verified by running the suites), not a byte-for-byte match of GNU as, whose
 * reorder heuristic differs.  Two passes over each reorder region:
 *
 *   - Branch/jump delay slot: hoist the preceding instruction into the slot
 *     when safe, otherwise a nop.  bc1f/bc1t always take a nop (the FP compare
 *     before them must stay put).
 *   - Load delay slot: a nop after a load or coprocessor move when the next
 *     instruction reads the loaded register.
 *
 * Macros (li, la, mul, div, ...) are single source lines here and are passed
 * through; the encoder expands them later, filling their own internal slots. */

#include "mips.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static const char *const CT_SET[] = {
    "j", "jal", "jalr", "jr", "b", "beq", "bne", "beqz", "bnez",
    "bc1f", "bc1t", NULL,
};
static const char *const HOIST_SET[] = {
    "addu", "subu", "and", "or", "xor", "nor",
    "sllv", "srav", "srlv", "sll", "sra", "srl",
    "slt", "sltu", "slti", "sltiu",
    "addiu", "andi", "ori", "xori", "lui", "move", "negu", "not",
    "sw", "sb", "sh", NULL,
};
static const char *const LOADD_SET[] = {   /* delayed dest is the 1st operand */
    "lw", "lb", "lbu", "lh", "lhu", "lwc1", "l.d", "l.s", "mfc1", "cfc1", NULL,
};

struct linevec {
    char **v;
    int n, cap;
};

static void
lv_push(struct linevec *L, char *s)
{
    if (L->n == L->cap) {
        L->cap = L->cap ? L->cap * 2 : 256;
        L->v = realloc(L->v, (size_t)L->cap * sizeof(char *));
        if (!L->v)
            die("mips_sched: out of memory");
    }
    L->v[L->n++] = s;
}

/* an instruction line starts with a tab and is not a directive (.foo) */
static int
is_insn_line(const char *l)
{
    return l[0] == '\t' && l[1] && l[1] != '.';
}

/* does the line's mnemonic equal m (whole token)? */
static int
mnem_is(const char *l, const char *m)
{
    size_t k;

    while (*l == '\t' || *l == ' ')
        l++;
    k = strlen(m);
    if (strncmp(l, m, k) != 0)
        return 0;
    return l[k] == '\0' || l[k] == '\t' || l[k] == ' ';
}

static int
mnem_in(const char *l, const char *const *set)
{
    for (; *set; set++)
        if (mnem_is(l, *set))
            return 1;
    return 0;
}

/* copy the n-th (0-based) $reg token of the line into buf; 1 if found */
static int
nth_reg(const char *l, int n, char *buf, int sz)
{
    const char *p = l;
    int idx = 0;

    while ((p = strchr(p, '$')) != NULL) {
        const char *s = p++;
        int len;
        while (*p && isalnum((unsigned char)*p))
            p++;
        if (idx == n) {
            len = (int)(p - s);
            if (len >= sz)
                len = sz - 1;
            memcpy(buf, s, (size_t)len);
            buf[len] = '\0';
            return 1;
        }
        idx++;
    }
    return 0;
}

/* does the line mention register reg as a whole token? */
static int
mentions(const char *l, const char *reg)
{
    size_t k = strlen(reg);
    const char *p = l;

    while ((p = strstr(p, reg)) != NULL) {
        if (!isalnum((unsigned char)p[k]))
            return 1;
        p += k;
    }
    return 0;
}

static int
is_delayed_load(const char *l)
{
    return mnem_in(l, LOADD_SET) || mnem_is(l, "mtc1");
}

/* the register a delayed load/move writes (mtc1 rt,fs writes fs, the 2nd) */
static int
load_dest(const char *l, char *buf, int sz)
{
    return nth_reg(l, mnem_is(l, "mtc1") ? 1 : 0, buf, sz);
}

/* Fill branch/jump delay slots (in -> out). */
static void
branch_pass(struct linevec *in, struct linevec *out, char *nopln)
{
    int i;

    for (i = 0; i < in->n; i++) {
        char *li = in->v[i];

        if (is_insn_line(li) && mnem_in(li, CT_SET)) {
            char *slot = NULL;
            int last = out->n - 1;

            if (!mnem_is(li, "bc1f") && !mnem_is(li, "bc1t") &&
                last >= 0 && is_insn_line(out->v[last]) &&
                mnem_in(out->v[last], HOIST_SET) &&
                /* out[last] must not itself be a just-filled delay slot */
                !(last >= 1 && is_insn_line(out->v[last - 1]) &&
                  mnem_in(out->v[last - 1], CT_SET))) {
                char def[24];
                int hasdef = nth_reg(out->v[last], 0, def, sizeof def);
                /* safe only if the branch does not read what the hoisted
                   instruction writes (e.g. jalr $t9 reads its target) */
                if (!hasdef || !mentions(li, def)) {
                    slot = out->v[last];
                    out->n--;
                }
            }
            lv_push(out, li);
            lv_push(out, slot ? slot : nopln);
        } else {
            lv_push(out, li);
        }
    }
}

/* Insert load-delay nops (in -> out). */
static void
load_pass(struct linevec *in, struct linevec *out, char *nopln)
{
    int i;

    for (i = 0; i < in->n; i++) {
        char *li = in->v[i];

        lv_push(out, li);
        if (is_insn_line(li) && is_delayed_load(li)) {
            char dest[24];
            int j = i + 1;

            /* the next executed instruction, past any labels/directives */
            while (j < in->n && !is_insn_line(in->v[j]))
                j++;
            if (load_dest(li, dest, sizeof dest) && j < in->n &&
                mentions(in->v[j], dest))
                lv_push(out, nopln);
        }
    }
}

/* Schedule one reorder region (in) onto out. */
static void
schedule_region(struct linevec *in, struct linevec *out, char *nopln)
{
    struct linevec mid = {0};

    branch_pass(in, &mid, nopln);
    load_pass(&mid, out, nopln);
    free(mid.v);
}

/* Fill the delay slots in the `.set reorder` regions of src, returning the
 * rewritten source (arena-allocated).  `.set noreorder` regions pass through. */
char *
mips_schedule(struct arena *a, const char *src)
{
    char *buf = xstrdup(src);
    struct linevec raw = {0}, out = {0};
    char nopln[] = "\tnop";
    char *p = buf;
    int i, reorder = 1;
    size_t total;
    char *res, *w;

    /* split into NUL-terminated lines (newline dropped, re-added on join) */
    while (*p) {
        char *nl = strchr(p, '\n');
        if (nl)
            *nl = '\0';
        lv_push(&raw, p);
        if (!nl)
            break;
        p = nl + 1;
    }

    /* walk regions, splitting at .set reorder / .set noreorder; a reorder run
       is scheduled as one region, a noreorder run is copied verbatim */
    i = 0;
    while (i < raw.n) {
        if (mnem_is(raw.v[i], ".set")) {
            if (strstr(raw.v[i], "noreorder"))
                reorder = 0;
            else if (strstr(raw.v[i], "reorder"))
                reorder = 1;
            lv_push(&out, raw.v[i++]);
            continue;
        }
        if (!reorder) {
            lv_push(&out, raw.v[i++]);
            continue;
        }
        /* gather a maximal reorder region up to the next .set, schedule it */
        struct linevec seg = {0};
        while (i < raw.n && !mnem_is(raw.v[i], ".set"))
            lv_push(&seg, raw.v[i++]);
        schedule_region(&seg, &out, nopln);
        free(seg.v);
    }

    /* join back into one string */
    total = 1;
    for (i = 0; i < out.n; i++)
        total += strlen(out.v[i]) + 1;
    res = arena_alloc(a, total);
    w = res;
    for (i = 0; i < out.n; i++) {
        size_t n = strlen(out.v[i]);
        memcpy(w, out.v[i], n);
        w += n;
        *w++ = '\n';
    }
    *w = '\0';

    free(raw.v);
    free(out.v);
    free(buf);
    return res;
}
