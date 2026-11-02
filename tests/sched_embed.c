/* sched_embed.c : drive the scheduler the way an embedding host (boris, smolmoo)
 * does, with no run_sandbox and no CLI.  Obtain the arch's CPU vtable from the
 * public getter, init a scheduler, add a root from an in-memory ELF image
 * (sched_root.image, not a path), run it to completion, and read the exit status
 * back through the public S.procs[] table.  Proves the ops-accessor getters and
 * the in-memory root together.
 */
#include "sched.h"
#include "guest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "build/rv/memroot.elf";
    FILE *f = fopen(path, "rb");
    long n;
    uint8_t *buf;
    run_opts o;
    sched S;
    sched_root r;
    char *av[2];
    int h, status;

    if (!f || fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0) {
        fprintf(stderr, "cannot read %s\n", path);
        if (f) fclose(f);
        return 2;
    }
    rewind(f);
    buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "cannot read %s\n", path);
        free(buf);
        fclose(f);
        return 2;
    }
    fclose(f);

    memset(&o, 0, sizeof o);
    o.mem_limit = 256u * 1024u * 1024u;
    o.stack_size = 1u * 1024u * 1024u;
    o.heap_size = 16u * 1024u * 1024u;
    av[0] = (char *)path;
    av[1] = NULL;
    o.path = path;
    o.argv = av;
    o.argc = 1;

    sched_init(&S, &o, skj_ops_rv32());     /* the getter: no exported static */
    memset(&r, 0, sizeof r);
    r.path = "<embedded>";                  /* a label; the bytes are in memory */
    r.image = buf;
    r.image_len = n;
    h = sched_add_root(&S, &r);
    if (h <= 0) {
        fprintf(stderr, "sched_add_root failed\n");
        return 1;
    }
    sched_run(&S);
    status = S.procs[0]->rc ? S.procs[0]->rc : S.procs[0]->g.exit_code;
    sched_destroy(&S);
    free(buf);

    if (status != 0) {
        fprintf(stderr, "guest status %d\n", status);
        return 1;
    }
    printf("embed ok\n");
    return 0;
}
