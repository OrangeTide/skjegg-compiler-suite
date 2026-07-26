type
    class F
        private
            func fib(n is int) returns int
                if n < 2 then
                    return n
                endif
                return fib(n - 1) + fib(n - 2)
            endfunc
        public
            verb main() returns int
                return fib(10)
            endverb
    endclass
