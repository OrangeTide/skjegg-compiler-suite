// exs_err_meta_error_arm.exs : `error(...)` in an `otherwise` arm is the
// quality lever of the no-bounds model (typed-macros.md D6): the macro rejects
// an argument in its own words instead of letting a raw post-expansion type
// error surface, and the message is reported at the call site, which is where
// the author who wrote the argument is looking.
macro double(v)
    match typeof(v)
        when int then quasi ${v} + ${v}
        otherwise error("double needs an int")
    endmatch
endmacro

type
    class C
        public
            verb main(player is obj) returns int
                player.tell("n ${double(true)}")
                return 0
            endverb
    endclass
