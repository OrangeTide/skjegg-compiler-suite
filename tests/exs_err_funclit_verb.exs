// exs_err_funclit_verb.exs : a func value cannot call a sibling verb
// (function-values.md D4). A bare sibling-verb call is a self-send, and a
// self-send needs `self`, which a func value may not capture. An explicit
// send to a receiver passed as a parameter is the actor-safe way.
type
    class C
        private
            hp is int = 50
        public
            verb hurt() returns int
                self.hp = self.hp - 1
                return self.hp
            endverb
            verb main(player is obj) returns int
                var f = func() returns int
                            return hurt()
                        endfunc
                player.tell("${f()}")
                return 0
            endverb
    endclass
