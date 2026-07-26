// exs_otherwise.exs : the value fallback operator `otherwise`
// (fallback-words.md). `else` is only the boolean branch; `otherwise`
// supplies a backup value when a fallible expression yields none, and it
// chains right-associatively as a cascade, first success wins.
// Made by a machine. PUBLIC DOMAIN (CC0-1.0)
type
    class C
        public
            verb main() returns int
                var xs is list<int> = [10 20 30]
                var r is int = 0

                r = r + (xs[2] otherwise 0)          // 20
                r = r + (xs[9] otherwise 5)          // 5: out of range

                // chaining: right-associative, lazy, first success wins
                r = r + (xs[9] otherwise xs[8] otherwise 1)   // 1: both fail
                r = r + (xs[9] otherwise xs[1] otherwise 1)   // 10: second wins

                // `else` (bool branch) and `otherwise` (fallback) together
                var hit is int = xs[9] otherwise 0   // 0
                if hit = 0 then
                    r = r + 100
                else
                    r = r + 1000
                endif

                return r                             // 20+5+1+10+100 = 136
            endverb
    endclass
