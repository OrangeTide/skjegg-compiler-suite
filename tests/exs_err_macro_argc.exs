// exs_err_macro_argc.exs : a macro call with the wrong number of arguments (meta.md)
type
    record Vec3
        x is int
        y is int
        z is int
    endrecord

macro m(v, w)
    quote 0
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
