// exs_err_if_fallible.exs : `if` and `while` are bool-only
// (fallible-consumers.md). A fallible condition is rejected so a failure can
// never collapse into `false`; the message names `if var` and `otherwise`.
type
    class C
        public
            verb main(player is obj) returns int
                var xs is list<int> = [1 2 3]
                if xs[9] then player.tell("yes") endif
                return 0
            endverb
    endclass
