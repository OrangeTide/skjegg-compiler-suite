// exs_match_expr.exs : the match construct in expression position. Same
// shape as the statement, arm bodies are single expressions, and all
// arms share one type. No match with no otherwise would trap (like select).
type
    class C
        private
            func name_of(n is int) returns str
                return match n
                    when 1 then "one"
                    when 2, 3 then "few"
                    otherwise "many"
                endmatch
            endfunc
        public
            verb main(player is obj) returns int
                player.tell("${name_of(1)}")
                player.tell("${name_of(3)}")
                player.tell("${name_of(7)}")
                var f is decimal = 1.5
                var x is int = match f
                    when 1.5 then 20
                    otherwise 0
                endmatch
                return x                   // 20
            endverb
    endclass
