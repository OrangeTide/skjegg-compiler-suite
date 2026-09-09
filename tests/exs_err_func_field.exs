// exs_err_func_field.exs : a func value in a field is a stored closure, which
// is deferred (function-values.md D4). Behavior on an object is stored as a
// verb or an included capability, not a func in a field.
type
    class C
        private
            cb is func(int) returns int
        public
            verb main(player is obj) returns int
                return 0
            endverb
    endclass
