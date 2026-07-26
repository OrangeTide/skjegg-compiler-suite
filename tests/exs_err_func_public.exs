// exs_err_func_public.exs : a func is a private helper, so it lives under
// `private`; the message names `verb` as the public alternative.
type
    class C
        public
            func helper() returns int
                return 1
            endfunc
            verb main(player is obj) returns int
                return 0
            endverb
    endclass
