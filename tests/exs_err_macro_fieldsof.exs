// exs_err_macro_fieldsof.exs : fieldsof(T) walks a record's fields
// (record-introspection.md). The sibling walks are verbsof for a class and
// membersof for an enum, so a non-record type is rejected here rather than
// silently yielding nothing.
macro m(v)
    var total = quote 0
    for f in fieldsof(typeof(v)) do
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
