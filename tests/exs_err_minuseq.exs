// exs_err_minuseq.exs : there are no compound assignments; write `x = x - 1`
type
    class C
        public
            verb main(player is obj) returns int
                var n is int = 1
                n -= 1
                return 0
            endverb
    endclass
