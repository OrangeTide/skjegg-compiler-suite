type
    class C
        public
            verb main(player is obj) returns int
                var s is str = "abc"
                player.tell(s[1] otherwise "?")
                player.tell(s[3] otherwise "?")
                player.tell(s[0] otherwise "?")
                player.tell(s[9] otherwise "?")
                var i is int = 2
                player.tell(s[i] otherwise "out")
                player.tell("mid ${s[2] otherwise "?"} end")
                return 0
            endverb
    endclass
