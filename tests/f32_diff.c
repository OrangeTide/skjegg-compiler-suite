/* f32_diff.c : cross-backend single-precision differential test.
 *
 * Compiled by skj-cc for each backend (its `float` is native IR_F32) and
 * run under that backend's qemu.  For every vector it injects the exact
 * operand bits through a union, performs the operation with C `float`
 * arithmetic (the same IR_F32 opcodes a pure-f32 Excelsior would emit),
 * extracts the exact result bits, and compares to the golden value the
 * RV32 emulator's f32 core produced (f32_golden.inc, via f32_oracle.c).
 *
 * Reporting rides the exit code, the way the other cc tests do: 0 means
 * every vector matched the oracle; a nonzero N means vector N-1 was the
 * first to diverge.  The runner maps N-1 back to a description through
 * build/f32_index.txt.  (Kept under 250 so it never collides with a
 * signal-derived 128+n code from qemu.)
 *
 * A divergence is a real determinism bug for any game that syncs f32
 * state across two peers running different backends.  Expected offenders:
 * ColdFire (its FPU computes in a wider internal format and cannot hold a
 * bit-exact binary32, so subnormal and NaN-payload vectors will differ),
 * and any backend whose runtime enables flush-to-zero (the subnormal
 * vectors turn to zero).
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "f32_golden.inc"

union pun {
    unsigned int u;
    float        f;
};

int
main(void)
{
    int i;

    for (i = 0; i < F32_N; i++) {
        int op = f32_op[i];
        union pun av, bv, rv;
        unsigned int got;

        if (op == F32_OP_I2F) {
            rv.f = (float)(int)f32_a[i];         /* signed int32 -> single */
            got = rv.u;
        } else if (op == F32_OP_F2I) {
            av.u = f32_a[i];
            got = (unsigned int)(int)av.f;       /* single -> signed int32 */
        } else {
            av.u = f32_a[i];
            bv.u = f32_b[i];
            if (op == F32_OP_EQ) {
                got = (av.f == bv.f) ? 1u : 0u;
            } else if (op == F32_OP_LT) {
                got = (av.f < bv.f) ? 1u : 0u;
            } else if (op == F32_OP_LE) {
                got = (av.f <= bv.f) ? 1u : 0u;
            } else {
                if (op == F32_OP_ADD)      rv.f = av.f + bv.f;
                else if (op == F32_OP_SUB) rv.f = av.f - bv.f;
                else if (op == F32_OP_MUL) rv.f = av.f * bv.f;
                else if (op == F32_OP_DIV) rv.f = av.f / bv.f;
                else                       rv.f = -av.f;    /* F32_OP_NEG */
                got = rv.u;
            }
        }

        if (got != f32_exp[i])
            return i + 1;
    }

    return 0;
}
