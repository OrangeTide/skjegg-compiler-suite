// exs_err_else_fallback.exs : `else` is only the boolean branch; a value fallback is `otherwise`
type
    class C
        public
            verb main(player is obj) returns int
                var xs is list<int> = [1 2 3]
                var n is int = xs[9] else 0
                return 0
            endverb
    endclass
