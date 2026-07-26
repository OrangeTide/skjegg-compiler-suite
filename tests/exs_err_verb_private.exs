// exs_err_verb_private.exs : a verb is a message others can send, so it lives
// under `public`; the message names `func` as the private alternative.
type
    class C
        private
            verb greet(player is obj)
                player.tell("hi")
            endverb
        public
            verb main(player is obj) returns int
                return 0
            endverb
    endclass
