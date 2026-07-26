// exs_match.exs : match statement with constant labels, value lists,
// ranges, a const label, multi-statement arms, and otherwise. Every arm
// opens with `when` (match-when.md D1), so an arm boundary is one keyword
// and a body line is never misread as a label. A match statement with no
// otherwise and no matching arm is a no-op.
const
    LIM = 9

type
    class C
        private
            func classify(player is obj, n is int) returns int
                match n
                    when 1, 2 then
                        player.tell("few")
                        return 10
                    when 3 to 5 then
                        player.tell("some")
                        return 20
                    when LIM then
                        player.tell("limit")
                        return 40
                    otherwise
                        player.tell("many")
                        return 30
                endmatch
                return 0
            endfunc
        public
            verb main(player is obj) returns int
                var r is int = 0
                r = r + classify(player, 2)        // few    10
                r = r + classify(player, 4)        // some   20
                r = r + classify(player, 9)        // limit  40
                r = r + classify(player, 7)        // many   30
                // no match, no otherwise: a no-op. The subject is bounded by
                // the first `when`, so the newline after it is optional (D2).
                match r when 999 then
                        r = 0
                endmatch
                return r                   // 100
            endverb
    endclass
