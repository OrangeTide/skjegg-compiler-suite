// exs_err_reduce_arity.exs : reduce takes three arguments (a list, an initial
// value, and a func value); a two-argument call is an error, not a confusing
// downstream one (function-values.md D7).
type
    class C
        public
            verb main(player is obj) returns int
                var xs is list<int> = [1 2 3]
                var r = reduce(xs, 0)
                return 0
            endverb
    endclass
