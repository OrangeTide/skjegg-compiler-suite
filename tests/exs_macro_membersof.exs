// exs_macro_membersof.exs : enum introspection, the fieldsof sibling
// (record-introspection.md). `membersof(T)` yields an enum's members in
// declaration order as descriptors, for an enum-driven macro. A member
// descriptor carries `.name`. Here `enumnames` lists the member words and
// `membercount` sums them. The macro reads the enum type with `typeof` of an
// enum-typed value; the members are read at expansion, so nothing about the
// introspection reaches runtime (enum values themselves are not lowered yet,
// but a member walk needs only the type).
type
    enum Color [red green blue]
    enum Suit [hearts diamonds clubs spades]

macro enumnames(v)
    var s = quote ""
    for m in membersof(typeof(v)) do
        s = quasi "${${s}}${${m.name}} "
    endfor
    s
endmacro

macro membercount(v)
    var n = quote 0
    for m in membersof(typeof(v)) do
        n = quasi ${n} + 1
    endfor
    n
endmacro

type
    class C
        public
            verb main(player is obj) returns int
                var c is Color
                var s is Suit
                player.tell(enumnames(c))                  // red green blue
                player.tell("colors ${membercount(c)}")    // 3
                player.tell("suits ${membercount(s)}")     // 4
                return 0
            endverb
    endclass
