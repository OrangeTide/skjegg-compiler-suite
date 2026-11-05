/* pas_rt.c - minimal Pascal runtime for the bare-metal PlayStation.
 *
 * The full runtime/pascal_rt.c does string and file I/O through a host libc. A
 * PlayStation game that drives libpsx directly needs almost none of that, so
 * this provides only the few symbols the Pascal prologue emits. It grows as a
 * sample needs more; a Pascal program that uses write/readln or managed strings
 * would pull in those pieces (a later milestone).
 */

/* The program prologue captures argc/argv for ParamStr/ParamCount. There are
 * no arguments on the PlayStation, so this is a no-op. */
void
__pascal_set_args(int argc, char **argv)
{
    (void)argc;
    (void)argv;
}
