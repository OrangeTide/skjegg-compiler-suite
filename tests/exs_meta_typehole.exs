// exs_meta_typehole.exs : `${T}` type holes (typed-macros.md D4). A `${}` in a
// type position splices a meta type value, so a template is parametric over a
// type. The type reaches the macro either as an argument (a builtin named at
// the call site, or a record name) or from `typeof` inside the body.
//
// The hole is substituted at expansion, so what the base checker sees is an
// ordinary cast to a concrete type. That matters: a written type inside a
// quote is carried through expansion, where before it was dropped and the
// cast silently became a cast to `any`, which assigns to anything.
type
    record Vec3
        x is int
    endrecord

// parametric over a type passed by the caller
macro convert(v, T)
    quasi ${v} as ${T}
endmacro

// or over a type the macro works out for itself
macro widen(v)
    quasi ${v} as ${typeof(v)}
endmacro

// a type hole and a type comparison in one body: the cast is emitted only
// where it means something
macro tostr(v, T)
    match typeof(v)
        when int, decimal, float then quasi "${${v} as ${T}}"
        otherwise error("tostr needs a number")
    endmatch
endmacro

type
    class C
        public
            verb main(player is obj) returns int
                var d is float = convert(3, float)
                var n is int = convert(3.5, int)
                var m is int = widen(7)
                player.tell("d ${d} n ${n} m ${m}")     // 3.000000 3 7 (a float prints its fraction)
                player.tell(tostr(9.5, int))            // 9
                return 0
            endverb
    endclass
