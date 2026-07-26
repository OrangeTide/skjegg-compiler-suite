// exs_macro_json.exs : a real serializer macro over record field annotations
// (record-annotations.md D6, meta if + tag inspection). `tojson` walks each
// field, reads its tags, renames the output key from a `json "..."` tag, and
// drops a field carrying a `skip` tag. It uses the meta-layer increment: a
// meta `if`, `not`, boolean vars, `=` on symbols, and tag `.head` / `.rest`
// inspection. The expansion is a plain string-concat chain; nothing about the
// tags survives to runtime.
type
    record Rec
        id is int, tags [json "id"]
        name is str, tags [json "name"]
        note is str
        secret is str, tags [skip]
    endrecord

macro tojson(v)
    var s = quote ""
    for f in fieldsof(typeof(v)) do
        var key = f.name
        var drop = false
        for t in f.tags do
            if t.head = "json" then
                key = t.rest.first          // rename to the json tag's value
            endif
            if t.head = "skip" then
                drop = true                 // omit this field
            endif
        endfor
        if not drop then
            s = quasi "${${s}}${${key}}=${get(${v}, ${f.name})} "
        endif
    endfor
    s
endmacro

type
    class C
        public
            verb main(player is obj) returns int
                var r is Rec = Rec(42, "Bob", "hi", "hunter2")
                // id and name renamed, note kept, secret dropped
                player.tell(tojson(r))       // id=42 name=Bob note=hi
                return 0
            endverb
    endclass
