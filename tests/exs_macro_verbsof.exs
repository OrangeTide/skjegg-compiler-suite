// exs_macro_verbsof.exs : class introspection, the fieldsof sibling
// (record-introspection.md). `verbsof(T)` yields a class's verbs in
// declaration order as descriptors, for a dispatch-table or proxy macro. A
// verb descriptor carries `.name`, `.returns` (the return type), and
// `.params` (a sequence of name/type descriptors). Here two macros walk them:
// `verbnames` lists the selectors, `totalparams` sums the parameter counts.
// Everything runs at expansion; nothing about the introspection reaches
// runtime.
type
    class Widget
        public
            verb poke(force is int) returns int
                return force + 1
            endverb
            verb nudge(dx is int, dy is int) returns int
                return dx + dy
            endverb
            verb reset() returns int
                return 0
            endverb
    endclass

macro verbnames(v)
    var s = quote ""
    for m in verbsof(typeof(v)) do
        s = quasi "${${s}}${${m.name}} "
    endfor
    s
endmacro

macro totalparams(v)
    var n = quote 0
    for m in verbsof(typeof(v)) do
        for p in m.params do
            n = quasi ${n} + 1
        endfor
    endfor
    n
endmacro

type
    class C
        public
            verb main(player is obj) returns int
                var w is Widget = spawn(Widget)
                player.tell(verbnames(w))                  // poke nudge reset
                player.tell("params ${totalparams(w)}")    // 3 (1 + 2 + 0)
                return 0
            endverb
    endclass
