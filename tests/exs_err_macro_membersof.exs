// exs_err_macro_membersof.exs : membersof(T) is the enum walk
// (record-introspection.md, enums.md), so it wants an enum type.
macro m(v)
    var total = quote 0
    for x in membersof(typeof(v)) do
        total = quasi ${total} + 1
    endfor
    total
endmacro

type
    class C
        public
            verb main(player is obj) returns int
                player.tell("v ${m(1)}")
                return 0
            endverb
    endclass
