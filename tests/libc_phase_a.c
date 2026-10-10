/* Phase A libc self-check: string, ctype, stdlib numeric/sort, and the
   snprintf formatter, plus the stdint/limits data-model headers.  Returns 42
   when every check passes, otherwise the number of failed checks (kept well
   under 42 so a failure never masquerades as success). */

#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <limits.h>
#include <stdbool.h>
#include <assert.h>

static int fails;

#define CHK(c) do { if (!(c)) fails++; } while (0)

static int
cmp_int(const void *a, const void *b)
{
    int x = *(const int *)a;
    int y = *(const int *)b;
    return (x > y) - (x < y);
}

int
main(void)
{
    char b[64];

    /* --- string.h --- */
    {
        char dst[8];
        memset(dst, 'z', sizeof dst);
        CHK(dst[0] == 'z' && dst[7] == 'z');
        memcpy(dst, "abc", 4);
        CHK(strcmp(dst, "abc") == 0);
        CHK(strlen("hello") == 5);
        CHK(strnlen("hello", 3) == 3);
        CHK(strncmp("abcd", "abce", 3) == 0);
        CHK(strncmp("abcd", "abce", 4) < 0);
        CHK(memcmp("abc", "abd", 3) < 0);
        CHK(memchr("abc", 'b', 3) != NULL);
        CHK(memchr("abc", 'x', 3) == NULL);
    }
    {
        char dst[8];
        memmove(dst, "overlap", 8);
        memmove(dst + 1, dst, 6);       /* forward overlap */
        CHK(dst[0] == 'o' && dst[1] == 'o' && dst[2] == 'v');
    }
    {
        char dst[8];
        strcpy(dst, "ab");
        strcat(dst, "cd");
        CHK(strcmp(dst, "abcd") == 0);
        strncpy(dst, "xy", 5);          /* pads the rest with NUL */
        CHK(dst[0] == 'x' && dst[2] == 0 && dst[4] == 0);
    }
    {
        const char *hay = "hello";      /* one array; skj-cc does not pool literals */
        CHK(strchr(hay, 'l') == strstr(hay, "ll"));
        CHK(strrchr(hay, 'l')[0] == 'l' && strrchr(hay, 'l')[1] == 'o');
    }
    CHK(strchr("abc", 'x') == NULL);
    CHK(strstr("abcabc", "cab") != NULL);
    CHK(strstr("abc", "xyz") == NULL);
    CHK(strspn("aabbc", "ab") == 4);
    CHK(strcspn("abcde", "dc") == 2);
    CHK(strpbrk("hello", "xl")[0] == 'l');

    /* --- ctype.h --- */
    CHK(isdigit('7') && !isdigit('a'));
    CHK(isalpha('Q') && isalpha('q') && !isalpha('0'));
    CHK(isalnum('5') && isalnum('z') && !isalnum(' '));
    CHK(isspace(' ') && isspace('\n') && !isspace('x'));
    CHK(isxdigit('F') && isxdigit('9') && !isxdigit('g'));
    CHK(isupper('A') && !isupper('a'));
    CHK(ispunct('!') && !ispunct('a') && !ispunct(' '));
    CHK(toupper('a') == 'A' && toupper('A') == 'A');
    CHK(tolower('Z') == 'z' && tolower('z') == 'z');

    /* --- stdlib.h numeric --- */
    CHK(abs(-7) == 7 && abs(7) == 7);
    CHK(labs(-9L) == 9L);
    CHK(atoi("-123") == -123);
    CHK(atoi("  42abc") == 42);
    CHK(atol("1000000") == 1000000L);
    {
        char *end;
        long v = strtol("0x1F zzz", &end, 0);
        CHK(v == 31 && *end == ' ');
        v = strtol("777", &end, 8);
        CHK(v == 511);
        unsigned long u = strtoul("4000000000", NULL, 10);
        CHK(u == 4000000000UL);
    }
    {
        div_t d = div(17, 5);
        CHK(d.quot == 3 && d.rem == 2);
    }

    /* --- stdlib.h qsort / bsearch --- */
    {
        int a[] = { 5, 2, 9, 1, 7, 3, 8, 4, 6, 0 };
        int n = (int)(sizeof a / sizeof a[0]);
        int i, ok = 1, key = 7;
        int *found;
        qsort(a, (size_t)n, sizeof a[0], cmp_int);
        for (i = 1; i < n; i++)
            if (a[i - 1] > a[i])
                ok = 0;
        CHK(ok && a[0] == 0 && a[9] == 9);
        found = bsearch(&key, a, (size_t)n, sizeof a[0], cmp_int);
        CHK(found != NULL && *found == 7);
        key = 42;
        CHK(bsearch(&key, a, (size_t)n, sizeof a[0], cmp_int) == NULL);
    }

    /* --- snprintf formatter --- */
    snprintf(b, sizeof b, "%d/%i/%u", -5, 5, 5u);
    CHK(strcmp(b, "-5/5/5") == 0);
    snprintf(b, sizeof b, "%5d|%-5d|%05d", 42, 42, 42);
    CHK(strcmp(b, "   42|42   |00042") == 0);
    snprintf(b, sizeof b, "%x %X %#x %o", 255u, 255u, 255u, 8u);
    CHK(strcmp(b, "ff FF 0xff 10") == 0);
    snprintf(b, sizeof b, "%d", INT_MIN);
    CHK(strcmp(b, "-2147483648") == 0);
    snprintf(b, sizeof b, "[%s][%.3s][%8s]", "hi", "hello", "r");
    CHK(strcmp(b, "[hi][hel][       r]") == 0);
    snprintf(b, sizeof b, "%c%c%%", 'A', 'B');
    CHK(strcmp(b, "AB%") == 0);
    snprintf(b, sizeof b, "%*d", 4, 7);
    CHK(strcmp(b, "   7") == 0);
    snprintf(b, sizeof b, "%lld", 9000000000LL);
    CHK(strcmp(b, "9000000000") == 0);
    snprintf(b, sizeof b, "%zu", (size_t)1234);
    CHK(strcmp(b, "1234") == 0);
    snprintf(b, sizeof b, "%p", (void *)0);
    CHK(strcmp(b, "(nil)") == 0);
    {
        char t[4];
        int n = snprintf(t, sizeof t, "hello");  /* truncation */
        CHK(n == 5 && strcmp(t, "hel") == 0);
    }

    /* --- stdint / limits / data model --- */
    CHK(CHAR_BIT == 8);
    CHK(sizeof(int) == 4 && sizeof(long long) == 8);
    CHK(sizeof(int32_t) == 4 && sizeof(int64_t) == 8 && sizeof(int16_t) == 2);
    CHK(INT_MAX == 2147483647);
    CHK(sizeof(size_t) == sizeof(void *) && sizeof(size_t) == sizeof(long));
#ifdef __skj_lp64__
    CHK(sizeof(long) == 8 && SIZE_MAX == UINT64_MAX);
#else
    CHK(sizeof(long) == 4 && SIZE_MAX == UINT32_MAX);
#endif

    /* --- assert (passing) --- */
    assert(1 + 1 == 2);

    return fails == 0 ? 42 : fails;
}
