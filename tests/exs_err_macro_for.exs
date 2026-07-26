// exs_err_macro_for.exs : a meta `for` iterates a field sequence, not a runtime range
type
    record Vec3
        x is int
        y is int
        z is int
    endrecord

macro m(v)
    var total = quote 0
    for f in 1 to 3 do
        total = quasi ${total} + 1
    endfor
    total
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
