type
    class C
        public
            verb main() returns int
                var a is float = 2.5
                var b is float = 2.5
                var total is int = 0
                if a = b then
                    total = total + 1
                endif
                if a < 3.0 and a > 2.0 then
                    total = total + 10
                endif
                if a <> 9.0 then
                    total = total + 100
                endif
                return total
            endverb
    endclass
