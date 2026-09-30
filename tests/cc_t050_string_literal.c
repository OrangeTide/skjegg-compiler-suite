/* cc_t050_string_literal.c : string literal access and char comparison */

int main(void)
{
    const char *s = "hello";
    int r = 0;

    if (s[0] == 'h')
        r += 1;
    if (s[4] == 'o')
        r += 2;

    char buf[6];
    for (int i = 0; i < 5; i++)
        buf[i] = s[i];
    buf[5] = '\0';

    if (buf[0] == 'h' && buf[4] == 'o')
        r += 4;

    return r;
}
