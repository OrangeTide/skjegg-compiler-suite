// exs_err_maybe_obj.exs : `maybe obj` is not a type, because nil already is
// the absent object (nil-nothing.md).
type
    class C
        public
            verb main(player is obj) returns int
                var who is maybe obj = nil
                return 0
            endverb
    endclass
