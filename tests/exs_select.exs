type
    class C
        public
            verb main(player is obj) returns int
                var i is int = 1
                player.tell((select i from "red", "green", "blue" otherwise "unknown"))
                player.tell((select 0 from "red", "green", "blue" otherwise "unknown"))
                player.tell((select 5 from "red", "green", "blue" otherwise "unknown"))
                var msg is str = select 2 from "zero", "one", "two"
                player.tell("${msg}")
                var pts is int = select i from 10, 20, 30 otherwise 0
                player.tell("${pts}")
                return 0
            endverb
    endclass
