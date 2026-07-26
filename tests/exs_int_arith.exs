type
    class Calc
        private
            func square(x is int) returns int
                return x * x
            endfunc
        public
            verb main() returns int
                var total is int = 0
                var i is int = 0
                for i in 1 to 5 do
                    total = total + square(i)
                endfor
                if total > 50 and total < 100 then
                    total = total - 13
                endif
                return total
            endverb
    endclass
