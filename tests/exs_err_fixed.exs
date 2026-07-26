// exs_err_fixed.exs : the binary `fixed` type was replaced by base-10 `decimal`
type
    class C
        public
            verb main(player is obj) returns int
                var d is fixed = 1.5
                return 0
            endverb
    endclass
