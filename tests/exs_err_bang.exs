// exs_err_bang.exs : negation is the word `not`, not `!`
type
    class C
        public
            verb main(player is obj) returns int
                var flag is bool = true
                if !flag then player.tell("no") endif
                return 0
            endverb
    endclass
