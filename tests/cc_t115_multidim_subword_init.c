/* cc_t115_multidim_subword_init.c : a multidimensional array of a sub-word
   element type must lay its static initializer out at the element's width.
   The flat initializer path took the IR element type from the array's
   immediate element (short[3] for short[4][3]), which is itself an array, so
   it defaulted to the word width and emitted one word per element.  The
   address arithmetic still used the true 2-byte stride, so every element past
   the first in a row read into the zero padding: v[0][1] came back 0, v[0][2]
   read v[0][1]'s intended value, and so on.  The element type is the scalar at
   the bottom of every dimension; the layout must match the stride.
*/

static const short VS[4][3] = {
    { 11, 12, 13 },
    { 21, 22, 23 },
    { 31, 32, 33 },
    { 41, 42, 43 },
};

static const signed char VC[2][3] = {
    { 1, 2, 3 },
    { 4, 5, 6 },
};

static const short V3[2][2][3] = {
    { { 1, 2, 3 }, { 4, 5, 6 } },
    { { 7, 8, 9 }, { 10, 11, 12 } },
};

/* a partial initializer: the rest of the aggregate is zero across every
   dimension, which only works when trailing zero-fill is sized in elements. */
static short VP[4][3] = { { 11, 12, 13 } };

int
main(void)
{
    if (VS[0][0] != 11) return 1;
    if (VS[0][1] != 12) return 2;         /* the element that read padding */
    if (VS[0][2] != 13) return 3;
    if (VS[3][2] != 43) return 4;

    if (VC[0][2] != 3)  return 5;
    if (VC[1][0] != 4)  return 6;
    if (VC[1][2] != 6)  return 7;

    if (V3[0][1][2] != 6)  return 8;
    if (V3[1][1][2] != 12) return 9;

    if (VP[0][2] != 13) return 10;
    if (VP[3][2] != 0)  return 11;        /* trailing zero-fill */
    if (VP[1][0] != 0)  return 12;

    return 42;
}
