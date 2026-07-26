// exs_err_macro_splice_type.exs : a ${} splice takes a form or a name, not a type
type
    record Vec3
        x is int
        y is int
        z is int
    endrecord

macro m(v)
    quasi ${typeof(v)}
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
