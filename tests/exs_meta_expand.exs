// exs_meta_expand.exs : a macro resolves during typechecking (typed-macros.md
// D2), so its arguments are already checked when it runs and its expansion is
// checked in its place. Two consequences this pins: a macro may emit a call to
// another macro (or to itself, terminating), and the name it emits resolves at
// the CALL SITE, since a built form was never parsed in a scope.
type
    record Vec3
        x is int
    endrecord

macro inner(v)
    match typeof(v)
        when int then quasi ${v} + 1
        when str then quasi ${v} + "!"
        otherwise error("inner needs an int or a str")
    endmatch
endmacro

// a macro whose expansion contains another macro call
macro outer(v)
    quasi inner(inner(${v}))
endmacro

// a macro call as a bare statement (its expansion is a call)
macro shout(p, s)
    quasi ${p}.tell(${s} + "!")
endmacro

type
    class C
        public
            verb main(player is obj) returns int
                player.tell("n ${outer(1)}")        // 3
                player.tell(outer("a"))             // a!!
                shout(player, "hi")                 // hi!
                return 0
            endverb
    endclass
