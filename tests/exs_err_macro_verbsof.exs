// exs_err_macro_verbsof.exs : verbsof(T) is the class walk, the fieldsof
// sibling (record-introspection.md), so it wants a class type.
macro m(v)
    var total = quote 0
    for x in verbsof(typeof(v)) do
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
