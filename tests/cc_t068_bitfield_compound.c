/* cc_t068_bitfield_compound.c : compound assignment and ++/-- on bit-fields */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */
struct T { unsigned a : 4; unsigned b : 6; int s : 5; };
int main(void) {
    struct T t;
    t.a = 3; t.b = 10; t.s = 2;
    int r = 0;
    t.a += 2;                 /* 5 */
    if (t.a == 5) r += 1;
    t.b -= 4;                 /* 6 */
    if (t.b == 6) r += 2;
    t.a *= 3;                 /* 15 */
    if (t.a == 15) r += 4;
    t.b |= 1;                 /* 7 */
    if (t.b == 7) r += 8;
    if (t.a++ == 15) r += 16; /* post: returns old 15; a wraps 16&15=0 */
    if (t.a == 0) r += 32;
    ++t.s;                    /* 3 */
    if (t.s == 3) r += 64;
    t.a <<= 1;                /* 0<<1 = 0; use b instead */
    t.b >>= 1;                /* 7>>1 = 3 */
    if (t.b == 3) r += 128;
    return (r == 255) ? 42 : (r & 63);
}
