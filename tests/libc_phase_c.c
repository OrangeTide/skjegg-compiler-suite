/* Phase C libc self-check: buffered stdio over the write/read seam.  The
   program writes a fixed script to stdout (compared against
   libc_phase_c.expected) and echoes stdin line by line (fed from
   libc_phase_c.input).  It also returns 42 only when its internal checks pass,
   so both the stream output and the control flow are verified. */

#include <stdio.h>
#include <string.h>

int
main(void)
{
    int ok = 1;
    size_t nw;
    char line[64];
    int ln = 0;

    /* printf / fprintf conversions */
    printf("hello %s, %d+%d=%d\n", "world", 2, 3, 5);
    printf("hex %x oct %o chr %c pct %%\n", 255u, 8u, 'Q');
    printf("pad [%5d] [%-5d] [%05d]\n", 42, 42, 42);
    fprintf(stdout, "long %ld\n", 1000000L);

    /* puts / fputs / fputc / putchar */
    puts("puts-line");
    fputs("fputs-part", stdout);
    putchar('\n');
    fputc('A', stdout);
    fputc('B', stdout);
    putchar('\n');

    /* fwrite returns the count of whole items written */
    nw = fwrite("fwrite12", 1, 8, stdout);
    putchar('\n');
    printf("wrote %d\n", (int)nw);
    if (nw != 8)
        ok = 0;

    /* echo stdin line by line with fgets, then report end-of-file */
    while (fgets(line, (int)sizeof line, stdin) != NULL) {
        printf("line %d: %s", ++ln, line);
        if (line[strlen(line) - 1] != '\n')
            putchar('\n');
    }
    printf("eof=%d lines=%d\n", feof(stdin) ? 1 : 0, ln);
    if (ln != 3 || !feof(stdin) || ferror(stdout))
        ok = 0;

    fflush(stdout);
    return ok ? 42 : 1;
}
