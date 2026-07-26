// exs_err_meta_overflow.exs : compile-time arithmetic is range-checked
// (meta-values.md D5). A macro runs before there is a runtime, so an overflow
// has nowhere to fault; it is an error at the macro that computed it.
macro m()
    var n = 2000000000 + 2000000000
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
