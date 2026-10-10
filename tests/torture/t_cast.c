/* Integer casts: truncation to a narrow type, sign versus zero extension back
   to a wide type, and the int/long long round trips. The bit pattern after each
   cast must match the host gcc oracle; t_hex prints the abstract value so width
   differences in the source never show, only a wrong extension or truncation
   would. */

#include "torture.h"

int
main(void)
{
    /* truncation to char then re-widening, signed and unsigned */
    {
        int big = 0x12345;
        signed char sc = (signed char)big;    /* 0x45 = 69 */
        unsigned char uc = (unsigned char)big; /* 0x45 */
        t_sint("to_sc", sc);                   /* 69 */
        t_sint("to_uc", uc);                   /* 69 */

        int neg = -1;
        signed char sn = (signed char)neg;     /* -1 */
        unsigned char un = (unsigned char)neg; /* 255 */
        t_sint("sc_neg", sn);                  /* -1, sign-extends */
        t_sint("uc_neg", un);                  /* 255, zero-extends */
    }

    /* short truncation */
    {
        int v = 0x7ffff;
        short s = (short)v;             /* 0xffff = -1 */
        unsigned short us = (unsigned short)v;
        t_sint("to_short", s);          /* -1 */
        t_sint("to_ushort", us);        /* 65535 */
    }

    /* int to long long: signed sign-extends, unsigned zero-extends */
    {
        int n = -5;
        unsigned u = 0x80000000u;
        long long sn = (long long)n;
        long long zn = (long long)u;    /* via unsigned: 0x0000000080000000 */
        t_hex("ll_sext", (unsigned long long)sn);  /* 0xfffffffffffffffb */
        t_hex("ll_zext", (unsigned long long)zn);  /* 0x0000000080000000 */
    }

    /* long long truncated to int keeps the low 32 bits */
    {
        long long w = 0x123456789aLL;
        int lo = (int)w;
        t_hex("ll_trunc", (unsigned long long)(unsigned)lo);  /* 0x3456789a */
    }

    /* a chain of casts through narrower and back */
    {
        int x = 300;
        int r = (int)(unsigned char)x;  /* 300 & 0xff = 44 */
        t_sint("chain", r);             /* 44 */
    }

    /* cast of an expression result */
    t_sint("expr", (signed char)(1000 + 29));   /* 1029 & 0xff = 5 */

    return 0;
}
