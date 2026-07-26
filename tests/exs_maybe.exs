// exs_maybe.exs : fallible expressions and maybe T (fallible.md).
// Failure propagates through expressions to the nearest consumer:
// otherwise supplies a default, if var branches and binds, while var
// loops until failure, capture into a maybe stores the outcome, and an
// unconsumed failure traps (or propagates out of a fallible func).
type
    class C
        private
            hits is maybe int = nothing
            counter is int = 3
            func half(n is int) returns maybe int
                if n % 2 = 1 then               // constant divisor: infallible then
                    fail
                endif
                return n / 2
            endfunc
            func validate(n is int) can fail
                if n < 0 then
                    fail
                endif
            endfunc
            func take() returns maybe int
                if counter = 0 then
                    fail
                endif
                counter = counter - 1
                return counter
            endfunc
        public
            verb main() returns int
                var r is int = 0
                var xs is list<int> = [10 20 30]

                r = r + (xs[2] * 2 otherwise 0)  // 40: propagation via *
                r = r + (xs[9] * 2 otherwise 1)  // 1: index fails, fallback

                r = r + (half(8) otherwise 100)  // 4
                r = r + (half(3) otherwise 100)  // 100: fail in the func

                if var i = half(10) then              // binds 5 then
                    r = r + i
                else
                    r = r + 1000
                endif
                if var j = half(7) then               // fails: else arm then
                    r = r + 1000
                else
                    r = r + 6
                endif

                validate(5) on fail r = r + 1000  // succeeds: handler skipped
                validate(-1) on fail r = r + 7    // fails: handler runs (+7)

                hits = half(12)                  // capture success (6)
                r = r + (hits otherwise 0)       // 6
                hits = half(5)                   // capture failure
                r = r + (hits otherwise 9)       // 9

                r = r + (find("or", "sword") otherwise 0)   // 3: 1-based
                if var p = find("xyz", "sword") then
                    r = r + 1000
                endif

                while var k = take() do             // 2, 1, 0 (boxed zero!) do
                    r = r + k + 1                // +3 +2 +1
                endwhile

                return r                         // 187
            endverb
    endclass
