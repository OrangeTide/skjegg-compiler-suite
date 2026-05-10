/* cc_t047_predecrement.c : pre-decrement as array index (tostr pattern) */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

int main(void)
{
    char buf[12];
    int pos = 12;
    int val = 907;

    while (val > 0) {
        buf[--pos] = '0' + (val % 10);
        val /= 10;
    }
    int len = 12 - pos;
    if (len != 3)
        return 1;
    if (buf[pos] != '9')
        return 2;
    if (buf[pos + 1] != '0')
        return 3;
    if (buf[pos + 2] != '7')
        return 4;
    return 42;
}
