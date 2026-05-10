#define HAS_FOO
#if defined(HAS_FOO) && !defined(HAS_BAR)
correct
#else
wrong
#endif
