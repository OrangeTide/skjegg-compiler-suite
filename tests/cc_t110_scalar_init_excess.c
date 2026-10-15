/* A braced initializer for a scalar type must hold a single value; excess
   elements are rejected (an assignment-type mismatch, not a silent drop). */
int g = {1, 2};

int
main(void)
{
    return g;
}
