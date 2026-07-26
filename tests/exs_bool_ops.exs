type
    class B
        private
            func test(a is int, b is int) returns int
                if a > 0 and not (b > 0) then
                    return 1
                endif
                if a > 0 or b > 0 then
                    return 2
                endif
                return 3
            endfunc
        public
            verb main() returns int
                var x is int = test(5, 0) * 10
                var y is int = test(0, 5)
                var z is int = test(0, 0)
                return x + y + z
            endverb
    endclass
