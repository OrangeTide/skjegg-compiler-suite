// exs_err_shared_arg.exs : a `shared` parameter takes the caller's own place
// by reference (shared-params.md D3/D4), so the argument must be a variable or
// a self field. A computed value has no place to write back to.
type
    class C
        private
            func bump(shared n is int)
                n = n + 1
            endfunc
        public
            verb main(player is obj) returns int
                var a is int = 1
                bump(a + 1)
                return 0
            endverb
    endclass
