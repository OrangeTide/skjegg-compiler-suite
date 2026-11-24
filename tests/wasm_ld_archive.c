/* wasm_ld_archive.c : exercises skj-ld-wasm archive member pull.
 *
 * It calls printf, malloc, free, strcpy, and strlen, which live in different
 * members of the WASM C runtime archive, so linking it against that archive
 * pulls exactly those members (and whatever they reference) and no others.
 * The expected stdout is "archive 42"; check-wasm-ld-archive also checks that
 * the archive-linked module is smaller than one linked against every member,
 * proving the unreferenced members were dropped. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
main(void)
{
    char *p = malloc(16);
    strcpy(p, "archive");
    printf("%s %d\n", p, (int)strlen(p) * 6);
    free(p);
    return 0;
}
