/* GNU C labels as values: &&label and the computed goto `goto *p`.  A
   token-threaded dispatch loop over a runtime-filled table of label addresses,
   the indirect-threading shape.  Returns (5 + 3) * 2 + 3 = 19. */
int main(void)
{
    static const int prog[] = { 0, 1, 0, 2 };   /* add, mul, add, halt */
    void *tab[3];
    int pc = 0;
    int acc = 5;

    tab[0] = &&op_add;      /* forward references: the labels come later */
    tab[1] = &&op_mul;
    tab[2] = &&op_halt;

    goto *tab[prog[pc]];

op_add:
    acc = acc + 3;
    pc = pc + 1;
    goto *tab[prog[pc]];

op_mul:
    acc = acc * 2;
    pc = pc + 1;
    goto *tab[prog[pc]];

op_halt:
    return acc;
}
