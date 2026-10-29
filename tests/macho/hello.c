/* hello.c : native macOS (Mach-O) smoke program for skj-cc-arm64.

   Exercises the Mach-O path end to end: a global, a const string, a local call,
   and a variadic libSystem call (printf, whose unnamed arguments ride the stack
   under the Darwin AArch64 ABI).  Prints exactly "hello macho 42". */

#include <stdio.h>

static const char *tag = "hello macho";
int base = 40;

static int add(int a, int b)
{
    return a + b;
}

int
main(void)
{
    printf("%s %d\n", tag, add(base, 2));
    return 0;
}
