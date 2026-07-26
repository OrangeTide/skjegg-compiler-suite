/* cc_t062_pointer_global_init.c : pointer globals initialized to an address */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

/* A pointer global initialized to an address constant becomes a symbol
   relocation: &var, an array name (decays), a string literal (a pointer to a
   string global, not inline bytes), a null pointer, and a static-local. */

int x = 40;
int arr[3] = { 10, 20, 30 };
int *p = &x;               /* bare address-of */
int *ap = arr;             /* array decays to pointer */
char *s = "hi";            /* char* to a string global (not inline bytes) */
int *np = 0;               /* null pointer stays 0 */

int counter(void) {
    static int *sp = &x;   /* static-local pointer relocation */
    return *sp;
}

int main(void) {
    int r = 0;
    r += *p;               /* 40 */
    r += ap[0] + ap[1] + ap[2];   /* 60 */
    r += s[0] + s[1];      /* 'h'+'i' = 209 */
    r += (np == 0) ? 1 : 0;/* null preserved -> 1 */
    r += counter();        /* 40 */
    return (r == 350) ? 42 : (r & 255);   /* 40+60+209+1+40 = 350 */
}
