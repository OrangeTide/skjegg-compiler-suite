#define CAT(a, b) a ## b
#define XCAT(a, b) CAT(a, b)
#define STR(x) #x
#define XSTR(x) STR(x)
#define MAKE_NAME(prefix, n) XCAT(prefix, n)
#define NAME_STR(prefix, n) XSTR(MAKE_NAME(prefix, n))
char *s = NAME_STR(item_, 5);
char *t = XSTR(XCAT(foo_, 3));
