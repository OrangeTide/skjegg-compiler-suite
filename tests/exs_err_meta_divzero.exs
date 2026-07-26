// exs_err_meta_divzero.exs : the same rule for division by zero
// (meta-values.md D5): a compile error at the macro, not a runtime fault.
macro m()
    var n = 1 / 0
    quasi ${n}
endmacro

type
    class C
        public
            verb main(player is obj) returns int
                player.tell("n ${m()}")
                return 0
            endverb
    endclass
