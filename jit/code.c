/* code.c : the growable machine-code byte buffer (see code.h). */

#include "code.h"

#include <stdlib.h>

void
code_init(struct code *c)
{
    c->cap = 256;
    c->buf = malloc(c->cap);
    c->len = 0;
}

void
code_free(struct code *c)
{
    free(c->buf);
    c->buf = NULL;
    c->len = c->cap = 0;
}

static void
grow(struct code *c, size_t need)
{
    if (c->len + need <= c->cap)
        return;
    while (c->len + need > c->cap)
        c->cap *= 2;
    c->buf = realloc(c->buf, c->cap);
}

void
emit8(struct code *c, uint8_t b)
{
    grow(c, 1);
    c->buf[c->len++] = b;
}

void
emit32(struct code *c, uint32_t v)
{
    grow(c, 4);
    c->buf[c->len++] = (uint8_t)v;
    c->buf[c->len++] = (uint8_t)(v >> 8);
    c->buf[c->len++] = (uint8_t)(v >> 16);
    c->buf[c->len++] = (uint8_t)(v >> 24);
}

void
emit64(struct code *c, uint64_t v)
{
    emit32(c, (uint32_t)v);
    emit32(c, (uint32_t)(v >> 32));
}
