/* cc_t059_float16.c : _Float16 storage type, arithmetic promoted to double */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

int ftoi_h(_Float16 x)
{
    return (int)x;
}

_Float16 addh(_Float16 a, _Float16 b)
{
    return a + b;
}

int main(void)
{
    int result = 0;

    /* storage size is 2 bytes */
    if (sizeof(_Float16) == 2)
        result += 1;

    /* half literal (f16 suffix), truncation to int */
    _Float16 h = 3.5f16;
    if (ftoi_h(h) == 3)
        result += 2;

    /* arithmetic promotes to double, result rounded back to a half slot */
    _Float16 a = 1.5f16;
    _Float16 b = 2.25f16;               /* exact in binary16 */
    if (ftoi_h(addh(a, b)) == 3)        /* 3.75 -> 3 */
        result += 4;

    /* array: exercises FSH (store) and FLH (load) through half memory */
    _Float16 arr[3];
    arr[0] = 1.5f16;
    arr[1] = 10.5f16;
    arr[2] = arr[0] + arr[1];           /* 12.0 */
    if (ftoi_h(arr[2]) == 12)
        result += 8;

    /* round-trip a value that is exact in binary16 */
    arr[0] = 0.5f16;
    if (ftoi_h(arr[0] + arr[0]) == 1)   /* 0.5 + 0.5 = 1.0 */
        result += 16;

    return result;                       /* expect 31 */
}
