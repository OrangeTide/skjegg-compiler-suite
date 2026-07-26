// exs_err_meta_fieldtype_value.exs : fieldtype takes a type and a name, so the same missing `typeof` is named
type
    record Vec3
        x is int
        y is int
    endrecord

macro m(v)
    var t = fieldtype(v, "x")
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
