// exs_err_meta_fieldsof_value.exs : the introspection walks take a type, and a macro parameter is bound to a
// value, so forgetting `typeof` is the mistake the message names
type
    record Vec3
        x is int
        y is int
    endrecord

macro m(v)
    var t = fieldsof(v)
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
