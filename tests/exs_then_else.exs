// exs_then_else.exs : the if-expression `cond then A else B`. General
// (any type, anywhere, not just string holes), the else is required,
// exactly one branch evaluates, and the else branch is a full expr so
// chains read like elseif. This replaced the old ${cond ? a : b} pick.
type
    class C
        public
            verb main(player is obj) returns int
                var locked is bool = true
                var lit is bool = false
                var n is int = 5
                player.tell("door is ${locked then "shut" else "open"}")
                player.tell("room is ${lit then "bright" else "dark"}")
                player.tell("n>3 is ${n > 3 then "big" else "small"}")
                player.tell("phrase ${locked then "firmly shut" else "open"}")
                var mood is str = lit then "beaming" else "gloomy"
                player.tell("${mood}")
                var damage is int = locked then n * 2 else n
                var size is str = (n > 9 then "huge"
                                   else n > 3 then "big"
                                   else "small")
                player.tell("${size}")
                var w is decimal = lit then 1.5 else 2.5
                player.tell("${w}")
                return damage              // 10
            endverb
    endclass
