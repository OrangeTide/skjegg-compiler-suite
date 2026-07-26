// exs_err_for_missing_do.exs : a `for` header is closed by `do` too, which is
// what marks the end of the iterable or of the `to`-range (then-in-if.md D2).
type
    class C
        public
            verb main(player is obj) returns int
                var total is int = 0
                for i in 1 to 4
                    total = total + i
                endfor
                return 0
            endverb
    endclass
