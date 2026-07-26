// exs_list_holes.exs : ${expr} holes in data literals. A hole whose
// expression folds to a constant joins the constant skeleton
// (`${2 + 3}` costs nothing at runtime); a runtime-valued hole patches
// its slot with one copy-on-write set. Holes join the element type.
const
    BASE = 30

type
    class C
        private
            func limit() returns int
                return 40
            endfunc
        public
            verb main(player is obj) returns int
                var xs is list<int> = [10 ${2 + 3} ${BASE} ${limit()}]
                var ss is list<str> = ["a" ${"b" + "c"}]
                var f is list<decimal> = [${1.5} 2.5]
                var sum is int = 0
                for v in xs do
                    sum = sum + v          // 10+5+30+40 = 85
                endfor
                player.tell(ss[2] otherwise "?")        // bc
                player.tell("${f[1] otherwise 0.0}")       // 1.500000
                return sum                 // 85
            endverb
    endclass
