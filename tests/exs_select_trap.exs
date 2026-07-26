type
    class C
        public
            verb main(player is obj) returns int
                var i is int = 9
                var s is str = select i from "a", "b"
                player.tell("${s}")
                return 0
            endverb
    endclass
