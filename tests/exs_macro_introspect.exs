// exs_macro_introspect.exs : record introspection in a compile-time macro
// (record-introspection.md, meta.md). A `macro` body runs at expansion:
// `fieldsof(typeof(v))` yields the record's fields, a compile-time `for`
// unrolls one copy of the body per field, and `quote`/`quasi` with `${}`
// splices assemble the output forms. `get(v, "field")` is the compile-time
// field accessor the walk emits. Nothing here survives to runtime: each call
// expands to plain field-access forms the base checker then validates.
type
    record Vec3
        x is int
        y is int
        z is int
    endrecord
    record Tagged
        name is str
        rank is int
    endrecord

// sum every field of a record (assumes int fields): expands to 0 + v.a + v.b
macro sumfields(v)
    var total = quote 0
    for f in fieldsof(typeof(v)) do
        total = quasi ${total} + get(${v}, ${f.name})
    endfor
    total
endmacro

// a serializer: "name=value " for each field, the canonical introspection use
macro describe(v)
    var s = quote ""
    for f in fieldsof(typeof(v)) do
        s = quasi "${${s}}${${f.name}}=${get(${v}, ${f.name})} "
    endfor
    s
endmacro

type
    class C
        public
            verb main(player is obj) returns int
                var a is Vec3 = Vec3(1, 2, 3)
                player.tell("sum ${sumfields(a)}")      // 6
                player.tell(describe(a))                 // x=1 y=2 z=3

                var t is Tagged = Tagged("orc", 5)
                player.tell(describe(t))                 // name=orc rank=5
                return 0
            endverb
    endclass
