// exs_macro_tags.exs : record field annotations, storage and the `.tags`
// accessor (record-annotations.md D1-D5). A field line is a comma list of
// entries; a `tags [...]` clause decorates the preceding field with a list of
// symbol-led atom entries. Tags are compile-time only and surface as
// `fieldsof`'s `.tags`. A macro walks them; here `tagcount` iterates
// `for t in f.tags` and emits `+ 1` per tag, so its expansion is the total
// tag count as a plain runtime expression. Full tag inspection (head/rest,
// `if`) is the next macro-interpreter increment (D6).
type
    record Point3D
        x is int = 0, tags [json "x", required]
        y is int = 0, tags [json "y", required]
        z is int = 0, tags [json "z", omitempty]
    endrecord
    record Mixed
        a is int = 0, b is int = 0, tags [order 2, key]
        c is str, tags [json "c", db "c_col" indexed]
    endrecord
    record Plain
        p is int = 0, q is int = 0
    endrecord

// count the tags across a record's fields: expands to 0 + 1 + 1 + ...
macro tagcount(v)
    var n = quote 0
    for f in fieldsof(typeof(v)) do
        for t in f.tags do
            n = quasi ${n} + 1
        endfor
    endfor
    n
endmacro

type
    class C
        public
            verb main(player is obj) returns int
                var pt is Point3D = Point3D(1, 2, 3)
                player.tell("p3d ${tagcount(pt)}")          // 6 (2+2+2)

                // a multi-field line: the tag decorates the preceding field b,
                // a is untagged; c carries two tags (one with three atoms)
                var m is Mixed = Mixed(1, 2, "hi")
                player.tell("mixed ${tagcount(m)}")          // 4 (0 + 2 + 2)

                var pl is Plain = Plain(7, 8)
                player.tell("plain ${tagcount(pl)}")         // 0

                // the record itself is unchanged: tags cost nothing at runtime
                player.tell("vals ${pt.x} ${m.c} ${pl.p}")   // 1 hi 7
                return 0
            endverb
    endclass
