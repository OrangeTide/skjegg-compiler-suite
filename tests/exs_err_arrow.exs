// exs_err_arrow.exs : `->` was removed by the de-arrow decision: words do structure
type
    class C
        public
            verb main(player is obj) returns int
                var n is int = 1 -> 2
                return 0
            endverb
    endclass
