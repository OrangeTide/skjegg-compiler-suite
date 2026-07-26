// exs_err_ifexpr_prec.exs : the if-expression binds loosest, so everything to
// the left of `then` is its condition. Here that swallows `1 + poisoned`, and
// the error lands on arithmetic the author wrote correctly, so the checker
// carries a note explaining the precedence. Must not compile.
type
    class C
        public
            verb main(player is obj) returns int
                var poisoned is bool = true
                var x is int = 0
                x = 1 + poisoned then 10 else 20
                player.tell("x ${x}")
                return 0
            endverb
    endclass
