/* Array and aggregate initializers whose elements are constant expressions or
   float literals, not bare int literals.  The flat init path only accepted int
   literals, negated int literals, strings, and addresses, so an int-element
   constant expression (1 * 2) was rejected and a float-element literal or
   expression (1.5, 1.0 / 4) had no element-type context to encode against.
   Each leaf now encodes against the element type.  Self-checking, returns 42. */

static int    ai[] = { 1 * 2, 3 + 4, 1 << 3, -5 };   /* 2, 7, 8, -5 */
static double ad[] = { 1.5, 2.5 + 1.0, 3, 1.0 / 2 }; /* 1.5, 3.5, 3.0, 0.5 */
static float  af[] = { 0.5f, 1.0f / 4, -2.0f };      /* 0.5, 0.25, -2.0 */
static int    m[2][2] = { { 1, 2 }, { 3 * 1, 2 + 2 } }; /* 1 2 3 4 */

struct point {
    double x;
    int    y;
};

static struct point p = { 1.5 + 0.5, 2 * 3 };       /* x = 2.0, y = 6 */

int
main(void)
{
    if (ai[0] != 2 || ai[1] != 7 || ai[2] != 8 || ai[3] != -5)
        return 1;
    if (ad[0] != 1.5 || ad[1] != 3.5 || ad[2] != 3.0 || ad[3] != 0.5)
        return 2;
    if (af[0] != 0.5f || af[1] != 0.25f || af[2] != -2.0f)
        return 3;
    if (m[0][0] != 1 || m[0][1] != 2 || m[1][0] != 3 || m[1][1] != 4)
        return 4;
    if (p.x != 2.0 || p.y != 6)
        return 5;
    return 42;
}
