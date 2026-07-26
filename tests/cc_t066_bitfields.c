/* cc_t066_bitfields.c : bit-field packing, read/write, signed extension */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */
struct flags {
    unsigned a : 3;   /* 0..7 */
    unsigned b : 5;   /* 0..31 */
    unsigned c : 1;
    int s : 4;        /* signed -8..7 */
};
int main(void) {
    struct flags f;
    f.a = 5; f.b = 20; f.c = 1; f.s = -3;
    int r = 0;
    if (f.a == 5) r += 1;
    if (f.b == 20) r += 2;
    if (f.c == 1) r += 4;
    if (f.s == -3) r += 8;                 /* signed sign-extension */
    f.a = 9;                                /* 9 & 7 = 1 (truncation) */
    if (f.a == 1) r += 16;
    f.b = f.b + 1;                          /* read-modify: 21 */
    if (f.b == 21) r += 32;
    if (sizeof(struct flags) == 4) r += 64; /* 3+5+1+4=13 bits -> one int unit */
    return (r == 127) ? 42 : (r & 63);
}
