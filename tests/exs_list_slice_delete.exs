type
    class C
        public
            verb main(player is obj) returns int
                var xs is list<int> = [10 20 30 40 50]
                var mid is list<int> = xs[2 to 4]
                player.tell("${length(mid)}")
                var s is int = 0
                for v in mid do
                    s = s + v
                endfor
                player.tell("${s}")

                var without is list<int> = delete(xs, 3)
                player.tell("${length(without)}")
                player.tell("${without[3] otherwise 0}")
                var s2 is int = 0
                for v in without do
                    s2 = s2 + v
                endfor
                player.tell("${s2}")

                var names is list<str> = ["a" "b" "c" "d"]
                var sub is list<str> = names[2 to 3]
                var joined is str = ""
                for c in sub do
                    joined = joined + c
                endfor
                player.tell("${joined}")

                var less is list<str> = delete(names, 1)
                player.tell(less[1] otherwise "?")
                player.tell("${length(less)}")

                var clamped is list<int> = xs[0 to 99]
                player.tell("${length(clamped)}")
                return 0
            endverb
    endclass
