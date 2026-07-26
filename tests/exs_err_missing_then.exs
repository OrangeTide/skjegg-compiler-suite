// exs_err_missing_then.exs : an `if` condition is closed by `then`
// (then-in-if.md D1/D6). The old then-less form reports at the head, naming
// the word to add, so the one-time migration is guided rather than silent.
type
    class C
        public
            verb main(player is obj) returns int
                var n is int = 3
                if n > 0
                    player.tell("yes")
                endif
                return 0
            endverb
    endclass
