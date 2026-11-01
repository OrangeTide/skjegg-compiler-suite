/* vdso_test.c : prove the vDSO injection seam from the guest side.
 *
 * Find the host-injected image by walking the auxv (environ points at the envp
 * array the crt set up; the auxv follows its NULL terminator), then call pure
 * leaf functions through the export table and check their results. Exits 0 on
 * success, a small nonzero code per failure.
 */
#include "skj_vdso.h"
#include <unistd.h>

extern char **environ;

static const struct skj_vdso *
find_vdso(void)
{
    unsigned long *p = (unsigned long *)environ;

    if (!p)
        return 0;
    while (*p)                  /* skip any env strings to the envp NULL */
        p++;
    p++;                        /* the auxv array follows */
    for (; p[0] != 0 /* AT_NULL */; p += 2)
        if (p[0] == AT_SKJ_VDSO)
            return (const struct skj_vdso *)(unsigned long)p[1];
    return 0;
}

int
main(void)
{
    const struct skj_vdso *v = find_vdso();
    uint32_t (*vstrlen)(const char *);
    int (*vstrcmp)(const char *, const char *);
    void *(*vmemcpy)(void *, const void *, uint32_t);
    uint64_t (*vmono)(void);
    uint64_t (*vreal)(void);
    char buf[8];

    if (!v) {
        write(2, "no vdso\n", 8);
        return 1;
    }
    if (v->magic != SKJ_VDSO_MAGIC || v->count < SKJ_VDSO_COUNT) {
        write(2, "bad table\n", 10);
        return 2;
    }

    vstrlen = (uint32_t (*)(const char *))v->fn[SKJ_VDSO_STRLEN];
    vstrcmp = (int (*)(const char *, const char *))v->fn[SKJ_VDSO_STRCMP];
    vmemcpy = (void *(*)(void *, const void *, uint32_t))v->fn[SKJ_VDSO_MEMCPY];
    vmono = (uint64_t (*)(void))v->fn[SKJ_VDSO_MONO_NS];
    vreal = (uint64_t (*)(void))v->fn[SKJ_VDSO_REAL_NS];

    if (vstrlen("hello") != 5) {
        write(2, "strlen\n", 7);
        return 3;
    }
    if (vstrcmp("abc", "abc") != 0 || vstrcmp("abc", "abd") >= 0) {
        write(2, "strcmp\n", 7);
        return 4;
    }
    vmemcpy(buf, "world", 6);
    if (vstrcmp(buf, "world") != 0) {
        write(2, "memcpy\n", 7);
        return 5;
    }

    /* The system page: the host keeps its clock updated and we read it with no
     * doorbell, both through the vDSO functions and directly off the page. */
    if (!v->sys || v->sys->magic != SKJ_VDSO_SYS_MAGIC) {
        write(2, "no syspage\n", 11);
        return 6;
    }
    if (vmono() == 0 || vreal() == 0) {
        write(2, "clock zero\n", 11);
        return 7;
    }
    /* realtime (since the epoch) is far larger than the monotonic uptime. */
    if (vreal() <= vmono()) {
        write(2, "clock order\n", 12);
        return 8;
    }
    /* The page carries more than the clock, QNX-style: a nonzero page size and
     * at least one cpu, read straight from the shared page. */
    if (v->sys->page_size == 0 || v->sys->ncpus == 0) {
        write(2, "syspage fields\n", 15);
        return 9;
    }

    write(1, "vdso ok\n", 8);
    return 0;
}
