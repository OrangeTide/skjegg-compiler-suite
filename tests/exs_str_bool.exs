type
    class C
        public
            verb main(player is obj) returns int
                var open is bool = true
                var n is int = 5
                player.tell("${open}")
                player.tell("door is ${open}")
                player.tell("locked is ${not open}")
                player.tell("${n} > 3 is ${n > 3}")
                player.tell("${n} = 3 is ${n = 3}")
                return 0
            endverb
    endclass
