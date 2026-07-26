// exs_err_int_div_widen.exs : an int/int quotient widening into decimal or
// float context is the truncation trap (numbers.md). The message names both
// spellings: a decimal operand, or an explicit `as`.
type
    class C
        public
            verb main(player is obj) returns int
                var d is decimal = 1 / 2
                player.tell("d ${d}")
                return 0
            endverb
    endclass
