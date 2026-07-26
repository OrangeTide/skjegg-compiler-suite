// exs_err_meta_verbsof_value.exs : verbsof takes a type; a bound macro parameter is a value (`typeof(v)`)
type
    record Vec3
        x is int
        y is int
    endrecord

macro m(v)
    var t = verbsof(v)
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
