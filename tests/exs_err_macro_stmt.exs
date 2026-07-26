// exs_err_macro_stmt.exs : the meta-language is small; a statement outside it is rejected in a macro body
type
    record Vec3
        x is int
        y is int
        z is int
    endrecord

macro m(v)
    return 0
endmacro

type
    class C
        public
            verb main(player is obj) returns int
                var a is Vec3 = Vec3(1, 2, 3)
                player.tell("v ${m(a)}")
                return 0
            endverb
    endclass
