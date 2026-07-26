// exs_err_len.exs : the `len` builtin was renamed `length` (list-ops.md)
type
    class C
        public
            verb main(player is obj) returns int
                var n is int = len("abc")
                return 0
            endverb
    endclass
