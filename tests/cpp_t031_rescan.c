#define CAT(a, b) a ## b
#define XCAT(a, b) CAT(a, b)
#define INC_0 1
#define INC_1 2
#define INC_2 3
#define INC(n) CAT(INC_, n)
#define PREFIX_1(x) first_ ## x
#define PREFIX_2(x) second_ ## x
#define DISPATCH(level, name) CAT(PREFIX_, level)(name)
int a = INC(2);
int b = XCAT(func_, INC(1));
DISPATCH(1, thing);
DISPATCH(2, other);
