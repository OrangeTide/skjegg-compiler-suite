int main(void)
{
    int a = 0xAA;
    int b = 0x55;
    int r = 0;
    if ((a & b) == 0) r += 1;
    if ((a | b) == 0xFF) r += 2;
    if ((a ^ b) == 0xFF) r += 4;
    if (~0 == -1) r += 8;
    if ((1 << 4) == 16) r += 16;
    if ((32 >> 2) == 8) r += 32;
    return r;
}
