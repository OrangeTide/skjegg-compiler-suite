// exs_err_missing_do.exs : a `while` header is closed by `do`, not `then`
// (then-in-if.md D2): `then` reads one-shot, `do` reads repeat.
type
    class C
        public
            verb main(player is obj) returns int
                var n is int = 3
                while n > 0
                    n = n - 1
                endwhile
                return 0
            endverb
    endclass
