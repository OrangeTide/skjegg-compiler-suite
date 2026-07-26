// exs_err_ternary.exs : the `?:` picker was removed; the if-expression replaces it
type
    class C
        public
            verb main(player is obj) returns int
                var flag is bool = true
                var s is str = flag ? "a" : "b"
                return 0
            endverb
    endclass
