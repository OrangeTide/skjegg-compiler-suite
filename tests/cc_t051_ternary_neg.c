/* cc_t051_ternary_neg.c : ternary with negation (tostr sign pattern) */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

int convert(int val)
{
    int neg = 0;
    if (val < 0) {
        neg = 1;
        val = -val;
    }
    int digit_sum = 0;
    while (val > 0) {
        digit_sum += val % 10;
        val /= 10;
    }
    return neg ? -digit_sum : digit_sum;
}

int main(void)
{
    int a = convert(123);
    int b = convert(-45);
    if (a != 6)
        return 1;
    if (b != -9)
        return 2;
    return a - b;
}
