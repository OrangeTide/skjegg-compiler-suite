// exs_err_view_field.exs : a field-restricted view `p is Box with (w, h)` may
// access only the named fields (record-slicing.md D4), checked at compile time.
type
    record Box
        w is int
        h is int
        tag is int
    endrecord
    class C
        private
            func grow(b is Box with (w, h))
                b.tag = 1
            endfunc
        public
            verb main(player is obj) returns int
                return 0
            endverb
    endclass
