type
    class C
        public
            verb main(player is obj) returns int
                var xs is list<int> = [10 20 30]
                player.tell("${xs[2]}")
                player.tell("${xs[1] otherwise 0}")
                player.tell("${xs[0] otherwise -1}")
                player.tell("${xs[9] otherwise -1}")
                var i is int = 3
                player.tell("${xs[i] otherwise 0}")
                var names is list<str> = ["red" "green" "blue"]
                player.tell(names[3] otherwise "?")
                player.tell(names[8] otherwise "none")
                player.tell("pick ${names[1] otherwise "?"}")
                return 0
            endverb
    endclass
