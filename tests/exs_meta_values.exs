// exs_meta_values.exs : the meta value domain (meta-values.md). A macro body
// computes with atoms (number, text, bool) beside the forms it carries, and
// an atom splices as the literal it is, so a macro can put a constant it
// computed into the program. `typeof` bridges either to a type. Everything
// here runs at expansion; nothing but the resulting literals reaches runtime.
type
    record Vec3
        x is int
        y is int
        z is int
    endrecord

// a computed constant: arithmetic on number atoms, spliced as a literal (D1,
// D4, D5). The expansion is the plain literal 14.
macro fourteen()
    var n = 2 + 3 * 4
    quasi ${n}
endmacro

// counting a record's fields at compile time: the meta `for` walks the field
// sequence and the count is an atom, so the call expands to `3`
macro fieldcount(v)
    var n = 0
    for f in fieldsof(typeof(v)) do
        n = n + 1
    endfor
    quasi ${n}
endmacro

// text `+` builds a generated name, and a bool atom splices as true/false
macro greeting()
    var s = "hello, " + "world"
    quasi ${s}
endmacro

macro bigger()
    var b = 2 > 1
    quasi ${b}
endmacro

// a meta `if` on a computed number picks one branch at expansion; the other
// never reaches the base checker
macro parity(v)
    var n = 0
    for f in fieldsof(typeof(v)) do
        n = n + 1
    endfor
    if n % 2 = 1 then
        quote "odd"
    else
        quote "even"
    endif
endmacro

type
    class C
        public
            verb main(player is obj) returns int
                var a is Vec3 = Vec3(1, 2, 3)
                player.tell("n ${fourteen()}")           // 14
                player.tell("fields ${fieldcount(a)}")   // 3
                player.tell(greeting())                  // hello, world
                player.tell("cmp ${bigger()}")           // true
                player.tell("parity ${parity(a)}")       // odd (3 fields)
                return 0
            endverb
    endclass
