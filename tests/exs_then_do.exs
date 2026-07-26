// exs_then_do.exs : a condition is closed by `then`, a loop header by `do`
// (then-in-if.md). The closer makes the header a newline-transparent region,
// so a compound condition wraps with no trailing-operator trick (D3), and it
// makes the one-line guard writable (D4). An if-expression sitting in a
// condition is parenthesized, and the outer `then` still closes the statement
// (D5).
type
    class C
        private
            // the one-line guard: `if C then STMT endif`, the body closed by
            // the block terminator in place of a newline (D4)
            func clamp(n is int) returns int
                if n < 0 then return 0 endif
                if n > 100 then return 100 endif
                return n
            endfunc

            // a compound condition wrapping across lines: `then`, not a
            // newline, closes it, so no line has to end on `and` (D3)
            func canCast(hp is int, mana is int, stunned is bool) returns bool
                if hp > 0
                   and mana >= 10
                   and not stunned then
                    return true
                endif
                return false
            endfunc

        public
            verb main(player is obj) returns int
                var total is int = 0

                player.tell("clamp ${clamp(-5)} ${clamp(50)} ${clamp(500)}")

                if canCast(10, 20, false) then
                    player.tell("cast")
                else
                    player.tell("no cast")
                endif
                if canCast(10, 5, false) then
                    player.tell("cast")
                else
                    player.tell("short on mana")
                endif

                // `do` closes a loop header, one-shot `then` versus repeat
                // `do` (D2); the inline body form works here too
                for i in 1 to 4 do
                    total = total + i
                endfor
                var n is int = 3
                while n > 0 do n = n - 1 endwhile
                player.tell("total ${total} n ${n}")

                // an if-expression as the condition is parenthesized, and the
                // outer `then` closes the statement (D5)
                if (total > 5 then true else false) then
                    player.tell("big")
                endif
                return 0
            endverb
    endclass
