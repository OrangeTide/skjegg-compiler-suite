// exs_div_ov_trap.exs : INT_MIN / -1 has no representable result and is
// an OVERFLOW fault (runtime-errors.md), never consumable: the `otherwise`
// must NOT catch it.
// Made by a machine. PUBLIC DOMAIN (CC0-1.0)

type
    class C
        public
            verb main() returns int
                var a is int = 0 - 2147483647 - 1
                var b is int = 0 - 1
                var q is int = a / b otherwise 5
                return q
            endverb
    endclass
