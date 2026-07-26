/* cc_t067_alignof.c : _Alignof operator and _Alignas on a global */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */
struct S { char c; double d; };
_Alignas(16) char buf[32];
int main(void) {
    int r = 0;
    if (_Alignof(char) == 1) r += 1;
    if (_Alignof(int) == 4) r += 2;
    if (_Alignof(double) == 8) r += 4;
    if (_Alignof(struct S) == 8) r += 8;
    if (alignof(long) == sizeof(long)) r += 16;      /* alignof alias */
    if (((unsigned long)(char*)buf & 15) == 0) r += 32; /* _Alignas(16) global */
    return (r == 63) ? 42 : (r & 31);
}
