// exs_err_ifexpr_bool.exs : the condition of `then ... else` must be bool.
// The message states the precedence rule, because a non-bool condition is
// usually a swallowed left side rather than a wrong type. Must not compile.
type
    class C
        public
            verb main(player is obj) returns int
                var hp is int = 3
                var x is int = 0
                x = hp then 10 else 20
                player.tell("x ${x}")
                return 0
            endverb
    endclass
