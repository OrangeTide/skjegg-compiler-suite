/* cc_t058_const_expr_array.c : constant expressions in array sizes */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#define WIDTH  8
#define HEIGHT 4
#define TOTAL  (WIDTH * HEIGHT)

int buf[3 + 5];
int grid[WIDTH * HEIGHT];
int mask[0xff & 0x03];
int ternary_arr[1 ? 4 : 8];

int main(void)
{
    int r = 0;

    /* 3 + 5 = 8 */
    if (sizeof(buf) / sizeof(buf[0]) == 8)
        r += 1;

    /* 8 * 4 = 32 */
    if (sizeof(grid) / sizeof(grid[0]) == 32)
        r += 2;

    /* define-based expression */
    int local[TOTAL];
    if (sizeof(local) / sizeof(local[0]) == 32)
        r += 4;

    /* bitwise: 0xff & 0x03 = 3 */
    if (sizeof(mask) / sizeof(mask[0]) == 3)
        r += 64;

    /* ternary: 1 ? 4 : 8 = 4 */
    if (sizeof(ternary_arr) / sizeof(ternary_arr[0]) == 4)
        r += 8;

    /* shift: 1 << 3 = 8 */
    int shifted[1 << 3];
    if (sizeof(shifted) / sizeof(shifted[0]) == 8)
        r += 16;

    /* subtraction and unary: 10 - 2 = 8 */
    int sub[10 - 2];
    if (sizeof(sub) / sizeof(sub[0]) == 8)
        r += 32;

    return r;
}
