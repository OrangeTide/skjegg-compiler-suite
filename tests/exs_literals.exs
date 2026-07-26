// exs_literals.exs : integer literal prefixes and exact fixed constants
type
    class Lit
        public
            verb main(player is obj) returns int
                var b is int = 0b1010              // 10
                var o is int = 0o17                // 15
                var h is int = 0X10                // 16
                var f is decimal = 2.5
                var g is decimal = 0.75              // exact
                player.tell("${f + g}")                         // 3.250000
                return b + o + h                   // 41
            endverb
    endclass
