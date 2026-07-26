// exs_err_macro_fieldtype.exs : fieldtype(T, "name") is a hard error when the
// field is absent (record-introspection.md; the fallible-consumer integration
// is deferred), so a typo in a macro's field name is caught at expansion.
type
    record Vec3
        x is int
        y is int
        z is int
    endrecord

macro m(v)
    quasi get(${v}, "x")
endmacro

macro bad(v)
    var t = fieldtype(typeof(v), "w")
    quote 0
endmacro

type
    class C
        public
            verb main(player is obj) returns int
                var a is Vec3 = Vec3(1, 2, 3)
                player.tell("v ${bad(a)}")
                return 0
            endverb
    endclass
