/* C11 _Static_assert: at file scope, inside a struct, and as a block item.
   All assertions use target-independent facts (int is 4, long long is 8 on
   every skj-cc target), so the file compiles and runs the same everywhere.
   The static_assert keyword spelling and the C23 no-message form are also
   exercised. */

_Static_assert(1 + 1 == 2, "arithmetic folds");
_Static_assert(sizeof(int) == 4, "int is four bytes");

enum { E_A = 3, E_B = 4 };
_Static_assert(E_A + E_B == 7, "enum constants fold");

struct packed {
    int a;
    _Static_assert(sizeof(long long) == 8, "long long is eight bytes");
    char b;
};

int
main(void)
{
    _Static_assert(2 > 1, "block-item static assert");
    static_assert(sizeof(char) == 1, "keyword spelling without a header");
    _Static_assert(40 + 2 == 42);   /* C23 no-message form */
    return 42;
}
