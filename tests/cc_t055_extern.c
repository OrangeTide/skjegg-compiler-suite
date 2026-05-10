/* cc_t055_extern.c : extern declarations and static functions */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

/* extern function declaration (forward) */
extern int add(int a, int b);

/* extern variable declaration, defined below */
extern int counter;

/* static helper (file-local linkage) */
static int double_it(int x)
{
    return x + x;
}

/* the definition for the extern variable */
int counter = 10;

/* function using extern declaration inside body */
int use_local_extern(void)
{
    extern int counter;
    return counter;
}

/* function that was declared extern above */
int add(int a, int b)
{
    return a + b;
}

int main(void)
{
    int result = 0;

    /* call function declared extern (forward decl) */
    if (add(3, 4) == 7)
        result += 1;

    /* read extern variable */
    if (counter == 10)
        result += 2;

    /* write extern variable */
    counter = 20;
    if (counter == 20)
        result += 4;

    /* call static function */
    if (double_it(5) == 10)
        result += 8;

    /* local extern declaration */
    if (use_local_extern() == 20)
        result += 16;

    /* extern function prototype with no extern keyword (implicit) */
    int sub(int, int);
    if (sub(9, 4) == 5)
        result += 32;

    return result;
}

int sub(int a, int b)
{
    return a - b;
}
