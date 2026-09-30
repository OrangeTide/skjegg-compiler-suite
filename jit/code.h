/* code.h : a growable machine-code byte buffer, shared by the JIT byte
   encoders (jit/emit_x86.c, jit/emit_arm64.c).

   The encoders append instruction bytes into a struct code; the JIT copies the
   finished bytes into an executable mapping. */

#ifndef JIT_CODE_H
#define JIT_CODE_H

#include <stddef.h>
#include <stdint.h>

/* A growable byte buffer that the encoders append into. Backed by malloc. */
struct code {
    uint8_t *buf;
    size_t len;
    size_t cap;
};

void code_init(struct code *c);
void code_free(struct code *c);
void emit8(struct code *c, uint8_t b);
void emit32(struct code *c, uint32_t v);
void emit64(struct code *c, uint64_t v);

#endif /* JIT_CODE_H */
