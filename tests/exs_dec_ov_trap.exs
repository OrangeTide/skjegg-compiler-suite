// exs_fx_ov_trap.exs : a fixed multiply whose result is outside the
// s15.16 range is an OVERFLOW fault, detected in the 64-bit host helper
// (runtime-errors.md).
// Made by a machine. PUBLIC DOMAIN (CC0-1.0)

type
    class C
        public
            verb main() returns int
                var a is decimal = 30000.0
                var b is decimal = 30000.0
                var c is decimal = a * b
                if c = 0.0 then
                    return 1
                endif
                return 2
            endverb
    endclass
