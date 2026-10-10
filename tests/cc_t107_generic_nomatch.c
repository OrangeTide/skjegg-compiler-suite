/* A _Generic with no matching association and no default is a translation-time
   error. The controlling type here is double, which no association names.
   This test must NOT compile (.experror). */
int
main(void)
{
    return _Generic(3.14, int: 1, char: 2);
}
