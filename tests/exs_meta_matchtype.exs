// exs_meta_matchtype.exs : `match typeof(x)`, compile-time multi-way type
// dispatch (typed-macros.md D3). One arm is chosen at expansion and only its
// body is emitted, so the dispatch costs nothing at runtime; this is the C
// `_Generic`. Labels are type values, so a value list (`when int, decimal`)
// and a named record label work the same way atom labels do, and an
// `otherwise error(...)` arm lets a macro reject an argument in its own words
// (D6's quality lever, reported at the call site).
type
    record Vec3
        x is int
        y is int
    endrecord

// dispatch on an argument's static type
macro describe(v)
    match typeof(v)
        when int, decimal then quote "a number"
        when str then quote "a str"
        when Vec3 then quote "a Vec3"
        otherwise error("describe needs a number, a str, or a Vec3")
    endmatch
endmacro

// the expression form of the same dispatch, and a match whose subject is an
// atom rather than a type: one construct, several subject kinds
macro sizeword(v)
    var n = 0
    for f in fieldsof(typeof(v)) do
        n = n + 1
    endfor
    var w = match n
                when 0 then "empty"
                when 1 to 2 then "small"
                otherwise "big"
            endmatch
    quasi ${w}
endmacro

// a generic operation: the caller writes an ordinary call and never sees the
// macro behind it (D1). Each site expands against its own argument's type.
macro double(v)
    match typeof(v)
        when int then quasi ${v} + ${v}
        when str then quasi ${v} + ${v}
        otherwise error("double needs an int or a str")
    endmatch
endmacro

type
    class C
        public
            verb main(player is obj) returns int
                var a is Vec3 = Vec3(1, 2)
                player.tell(describe(1))          // a number
                player.tell(describe("x"))        // a str
                player.tell(describe(a))          // a Vec3
                player.tell(sizeword(a))          // small (2 fields)
                player.tell("n ${double(21)}")    // 42
                player.tell(double("ab"))         // abab
                return 0
            endverb
    endclass
