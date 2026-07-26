// exs_err_typehole_value.exs : a `${}` in a type position splices a type
// (typed-macros.md D4), so an atom or a form there is a mistake. The error
// distinguishes it from a value hole, which is the same spelling in an
// expression position.
macro convert(v, T)
    quasi ${v} as ${T}
endmacro

type
    class C
        public
            verb main(player is obj) returns int
                var d is float = convert(3, 7)
                player.tell("d ${d}")
                return 0
            endverb
    endclass
