// exs_err_typelit_value.exs : naming a type where a value belongs is only
// meaningful to a macro parametric over it (typed-macros.md D4). Reaching an
// ordinary call, a type argument is a mistake worth naming, since the parser
// accepts the spelling in an argument position.
type
    class C
        private
            func takes(n is int) returns int
                return n
            endfunc
        public
            verb main(player is obj) returns int
                player.tell("n ${takes(int)}")
                return 0
            endverb
    endclass
