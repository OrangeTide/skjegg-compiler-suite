// exs_err_colon_send.exs : the colon send was removed; a send is `recv.verb(args)`
type
    class C
        public
            verb main(player is obj) returns int
                player:tell("hi")
                return 0
            endverb
    endclass
