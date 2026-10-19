/* Fixture for test-cc-rv-loc: debug-info line markers (-g).  Each statement
   below sits on its own line so the compiler emits one IR_LOC per line and the
   RISC-V backend records a "# line N" marker.  The test only inspects the
   assembly, so the program need not do anything interesting. */
int add(int a, int b)
{
    int s = a + b;
    return s;
}

int main(void)
{
    int x = add(2, 3);
    return x;
}
