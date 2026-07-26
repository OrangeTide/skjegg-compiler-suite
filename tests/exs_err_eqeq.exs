// exs_err_eqeq.exs : equality is `=`, not `==` (equality.md)
type
    class C
        public
            verb main(player is obj) returns int
                var n is int = 1
                if n == 1 then player.tell("one") endif
                return 0
            endverb
    endclass
