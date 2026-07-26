// exs_err_match_when.exs : every match arm opens with `when` (match-when.md
// D1). A label sitting where the arm word belongs is the old spelling, so the
// message names the arm shape. Must not compile.
type
    class C
        public
            verb main(player is obj) returns int
                var n is int = 2
                match n
                    1 then player.tell("one")
                    otherwise player.tell("many")
                endmatch
                return 0
            endverb
    endclass
