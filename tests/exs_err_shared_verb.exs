// exs_err_shared_verb.exs : `shared` is func-only (shared-params.md D5). A
// send crosses the actor boundary, where a by-reference place is meaningless,
// so the message points at a func or a returned value instead.
type
    class C
        public
            verb bump(shared n is int)
                n = n + 1
            endverb
            verb main(player is obj) returns int
                return 0
            endverb
    endclass
