/* cc_t114_enum_const_expr.c : an enumerator initializer is a full constant
   expression, not just a bare literal.  The parser once read the initializer
   node's ival directly, which is set only for an integer literal, so every
   computed enumerator (a shift, a bitwise or arithmetic combination, or a
   reference to a prior enumerator) silently became 0.  Fold it instead.
*/

enum Flags {
    F0 = 1 << 0,          /* 1  */
    F1 = 1 << 1,          /* 2  */
    F2 = 1 << 2,          /* 4  */
    F7 = 1 << 7,          /* 128 */
    BOTH = F0 | F2,       /* 5, reference + bitwise */
    NEXT,                 /* 6, implicit increment off a computed value */
    SUM = F1 + F2 * 3,    /* 14, arithmetic with precedence */
};

enum { A = 1 << 4, B = A - 1 };   /* anonymous enum, 16 and 15 */

int
main(void)
{
    if (F0 != 1)    return 1;
    if (F1 != 2)    return 2;
    if (F2 != 4)    return 3;
    if (F7 != 128)  return 4;
    if (BOTH != 5)  return 5;
    if (NEXT != 6)  return 6;
    if (SUM != 14)  return 7;
    if (A != 16)    return 8;
    if (B != 15)    return 9;
    return 42;
}
