/* A failing _Static_assert is a translation-time error naming the message.
   This test must NOT compile (.experror). */
_Static_assert(sizeof(int) == sizeof(long long), "int and long long differ in size");

int
main(void)
{
    return 0;
}
