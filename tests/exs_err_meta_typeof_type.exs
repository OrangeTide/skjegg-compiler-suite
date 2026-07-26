// exs_err_meta_typeof_type.exs : typeof bridges a value to its type, so it takes a value, not a type
type
    record Vec3
        x is int
        y is int
    endrecord

macro m(v)
    var t = typeof(typeof(v))
    quote 0
endmacro

type
    class C
        public
            verb main(player is obj) returns int
                var a is Vec3 = Vec3(1, 2)
                player.tell("v ${m(a)}")
                return 0
            endverb
    endclass
