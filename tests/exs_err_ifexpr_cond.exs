// exs_err_ifexpr_cond.exs : an if-expression written bare in a condition
// puts two `then`s on one line (then-in-if.md D5). The rule it breaks is the
// general one: an if-expression binds loosest, so it is parenthesized when it
// is not the whole expression. Must not compile.
type
    class C
        public
            verb main(player is obj) returns int
                var n is int = 3
                if n > 0 then true else false then
                    player.tell("yes")
                endif
                return 0
            endverb
    endclass
