type
    class C
        public
            verb main() returns int
                var a is decimal = 3.5
                var b is decimal = 0.25
                var c is decimal = a + b
                var d is decimal = c * 4
                var e is decimal = d / 2.0
                return e as int
            endverb
    endclass
