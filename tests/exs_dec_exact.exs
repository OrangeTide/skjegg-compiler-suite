// exs_dec_exact.exs : base-10 arithmetic does what the reader expects
// (numbers.md). decimal is value x 10^-4 in an int32, so every literal
// of up to four fraction digits is exact: 0.1 * 3 = 0.3, ten 0.1s are
// exactly 1, and printing is exact with trailing zeros stripped.
// Inference lands on decimal (whole numbers are int, decimals are
// decimal, float is asked for by name).
// Made by a machine. PUBLIC DOMAIN (CC0-1.0)

type
    class C
        public
            verb main(player is obj) returns int
                var price = 0.1
                if price * 3 = 0.3 then
                    player.tell("exact")
                endif
                var sum is decimal = 0.0
                for i in 1 to 10 do
                    sum = sum + price
                endfor
                player.tell("${sum}")
                player.tell("${price}")
                player.tell("${0.1 + 0.2}")
                player.tell("${2.5 * 2.5}")
                player.tell("${1.0 / 3.0}")
                player.tell("${0 - 1.5}")
                var w is decimal = 3.0
                player.tell("${w}")
                match price * 10
                    when 1.0 then player.tell("one")
                    otherwise player.tell("not one")
                endmatch
                var q is decimal = 7.5 % 2.0     // constant divisor: infallible
                player.tell("${q}")
                return 0
            endverb
    endclass
