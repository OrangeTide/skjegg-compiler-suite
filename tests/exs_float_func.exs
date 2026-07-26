type
    class C
        private
            func avg(a is float, b is float) returns float
                return (a + b) / 2.0
            endfunc
            func scale(x is float, k is int) returns float
                return x * k
            endfunc
        public
            verb main() returns int
                var m is float = avg(10.0, 20.0)
                var s is float = scale(m, 3)
                return s as int
            endverb
    endclass
