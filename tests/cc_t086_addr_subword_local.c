/* The address of a sub-word scalar local must reach the byte(s) the local's
   own loads and stores use, on either endianness.  Regression: on big-endian
   ColdFire a char/short local was kept in a word-rounded slot but &local
   pointed at the slot base (the high byte), so a byte store through the
   pointer missed the value byte. */

static void setc(char *p)  { *p = 'Q'; }
static void sets(short *p) { *p = 0x1234; }

int
main(void)
{
    char c = 7;
    short s = 1;
    setc(&c);
    sets(&s);
    if ((unsigned char)c != 'Q')
        return 1;
    if ((unsigned short)s != 0x1234)
        return 2;
    return 42;
}
