// exs_meta_typecmp.exs : compile-time type comparison, the dispatch test a
// typed macro is built on (typed-macros.md). `typeof(x) = int` tests against a
// builtin type named in the macro body, and `typeof(a) = typeof(b)` asks
// whether two arguments have the same type. Comparison is nominal, like the
// base layer: Vec3 and Other have identical fields and are still different
// types. One branch survives expansion; the others never reach the checker.
type
    record Vec3
        x is int
        y is int
    endrecord
    record Other
        x is int
        y is int
    endrecord

// dispatch on an argument's static type: one branch survives expansion. A
// builtin type names itself here, and so does a declared record.
macro describe(v)
    if typeof(v) = int then
        quote "an int"
    elseif typeof(v) = str then
        quote "a str"
    elseif typeof(v) = Vec3 then
        quote "a Vec3"
    else
        quote "something else"
    endif
endmacro

// a meta binding shadows a type of the same name, since the expander looks in
// its own environment first
macro shadowed(Vec3)
    if typeof(Vec3) = int then
        quote "the parameter won"
    else
        quote "the type won"
    endif
endmacro

// two arguments of the same type?
macro same(a, b)
    if typeof(a) = typeof(b) then
        quote "same"
    else
        quote "different"
    endif
endmacro

type
    class C
        public
            verb main(player is obj) returns int
                var a is Vec3 = Vec3(1, 2)
                var b is Other = Other(1, 2)
                var c is Vec3 = Vec3(3, 4)
                player.tell(describe(1))
                player.tell(describe("x"))
                player.tell(describe(a))
                player.tell(describe(b))
                player.tell(same(a, c))
                player.tell(same(a, b))
                player.tell(shadowed(7))
                return 0
            endverb
    endclass
