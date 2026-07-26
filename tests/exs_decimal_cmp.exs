type
    class C
        public
            verb main() returns int
                var a is decimal = 3.5
                var b is decimal = 2.5
                var total is int = 0
                if a > b then
                    total = total + 100
                endif
                if a >= 3.5 then
                    total = total + 10
                endif
                if a = 3.5 then
                    total = total + 1
                endif
                if a < b then
                    total = total + 1000
                endif
                return total
            endverb
    endclass
