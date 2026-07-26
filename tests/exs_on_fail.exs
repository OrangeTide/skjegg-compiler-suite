// exs_on_fail.exs : the `on fail` statement handler (fallible-consumers.md).
// The statement-level twin of the value fallback `otherwise` (fallback-words.md):
// run the left action; on success skip
// the handler; on failure run it (consuming the failure). Covers a bare
// `can fail` call, index recovery on an assignment, the valueless loop
// built from `while true` + `on fail break`, and the early-return guard.
// Made by a machine. PUBLIC DOMAIN (CC0-1.0)
type
    class C
        private
            counter is int = 3
            func take() can fail
                if counter = 0 then
                    fail                // nothing left to take
                endif
                counter = counter - 1
            endfunc
            func validate2(n is int) can fail
                if n = 0 then
                    fail
                endif
            endfunc
            func guard(n is int) returns int
                validate2(n) on fail return 0   // early-return on failure
                return 100                       // reached only on success
            endfunc
        public
            verb main() returns int
                var r is int = 0
                var xs is list<int> = [10 20 30]
                var x is int = 0

                take() on fail r = r + 1000      // succeeds: handler skipped
                r = r + 1                        // counter 2, r = 1

                x = xs[9] on fail x = 5          // index fails: handler runs
                r = r + x                        // r = 6

                while true do
                    take() on fail break         // drain until take() fails
                    r = r + 10                   // +10 +10 -> r = 26
                endwhile

                r = r + guard(1)                 // success path: 100 -> 126
                r = r + guard(0)                 // failure: early return 0

                return r                         // 126
            endverb
    endclass
