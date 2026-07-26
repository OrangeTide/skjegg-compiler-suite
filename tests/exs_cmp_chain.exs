// exs_cmp_chain.exs : chained comparisons. `a < b < c` means
// `a < b and b < c`: each operand evaluates once, and the chain
// short-circuits, so operands after the first false compare never run.
type
    class C
        private
            func mid(player is obj) returns int
                player.tell("m")
                return 2
            endfunc
            func probe(player is obj) returns int
                player.tell("p")
                return 99
            endfunc
        public
            verb main(player is obj) returns int
                var r is int = 0
                if 1 < 2 < 3 then
                    r = r + 1              // true
                endif
                if 1 < 2 < 2 then
                    r = r + 10             // false: 2 < 2 fails
                endif
                if 3 >= 2 >= 2 then
                    r = r + 2              // true
                endif
                if 7 = 7 = 7 then
                    r = r + 4              // true
                endif
                if 1 <= 2 < 3 then
                    r = r + 8              // true: <= and < share a direction
                endif
                if 1 < mid(player) < probe(player) then     // logs m then p, true then
                    r = r + 16
                endif
                if 9 < mid(player) < probe(player) then     // logs m only: 9 < 2 fails then
                    r = r + 100
                endif
                var f is decimal = 1.5
                if 1.0 < f < 2.0 then
                    r = r + 32             // fixed operands chain too
                endif
                return r                   // 1+2+4+8+16+32 = 63
            endverb
    endclass
