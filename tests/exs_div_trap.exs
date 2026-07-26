// exs_div_trap.exs : a bare divide by zero is a DIV_ZERO fault
// (runtime-errors.md): nothing consumed the failure, so the trap
// reports it and exits 70.
// Made by a machine. PUBLIC DOMAIN (CC0-1.0)

type
    class C
        public
            verb main() returns int
                var a is int = 10
                var z is int = 0
                var q is int = a / z
                return q
            endverb
    endclass
