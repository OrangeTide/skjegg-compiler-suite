/* C callback stubs for the Kobold FFI callback test. The Kobold side hands its own
   routines to C, which calls them back, and C hands a function pointer back to Kobold,
   which calls it. A Kobold procedure value is a bare code pointer emitted through the
   platform psABI, the same shape a C function pointer has, so neither direction needs a
   trampoline. */

/* C calls a Kobold function and returns its result. */
int apply_cb(int (*f)(int), int n)
{
    return f(n);
}

/* C calls a Kobold procedure (a void callback) once per index. */
void each(void (*f)(int), int n)
{
    int i;
    for (i = 0; i < n; i++)
        f(i);
}

/* C stores a Kobold callback and fires it later: the register-a-callback pattern a
   library such as SDL or raylib uses. */
static void (*g_cb)(int);

void set_cb(void (*f)(int))
{
    g_cb = f;
}

void fire(int x)
{
    if (g_cb)
        g_cb(x);
}

/* C returns a function pointer for Kobold to call. */
static int tripler(int x)
{
    return x * 3;
}

int (*get_fn(void))(int)
{
    return tripler;
}

/* void(void) and int(void) callbacks: the targets are parameterless Kobold routines
   whose address is taken with `@`. */
void call_v(void (*f)(void))
{
    f();
}

int call_i(int (*f)(void))
{
    return f();
}
