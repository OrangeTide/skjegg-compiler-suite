type
    class C
        public
            verb main(player is obj) returns int
                var xs is list<int> = []
                player.tell("${length(xs)}")
                xs = append(xs, 10)
                xs = append(xs, 20)
                xs = append(xs, 30)
                player.tell("${length(xs)}")
                player.tell("${xs[2] otherwise 0}")
                xs = set(xs, 2, 99)
                player.tell("${xs[2] otherwise 0}")
                player.tell("${xs[1] otherwise 0}")
                var names is list<str> = ["a" "b"]
                names = append(names, "c")
                player.tell("${length(names)}")
                player.tell(names[3] otherwise "?")
                names = set(names, 1, "z")
                player.tell("first ${names[1] otherwise "?"}")
                return 0
            endverb
    endclass
