// exs_int_ov_trap.exs : an int multiply whose result exceeds the signed
// 32-bit range is an OVERFLOW fault (runtime-errors.md), never consumable.
// The operands are variables, so the product is computed at runtime (IR_MULO)
// rather than folded.

type
    class C
        public
            verb main() returns int
                var a is int = 100000
                var b is int = 100000
                var c is int = a * b
                return c
            endverb
    endclass
