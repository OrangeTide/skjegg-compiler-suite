// exs_err_signal_value.exs : a `can fail` call is a valueless signal, not a
// bool (can-fail.md R14). Using it as a value names its consumers, and names
// `returns bool` for the case where a boolean fact was actually wanted.
type
    class C
        private
            counter is int = 1
            func take() can fail
                if counter = 0 then fail endif
                counter = counter - 1
            endfunc
        public
            verb main(player is obj) returns int
                var ok is bool = take()
                return 0
            endverb
    endclass
