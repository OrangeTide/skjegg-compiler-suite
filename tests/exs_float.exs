type
    class C
        public
            verb main() returns int
                var x is float = 3.5
                var y is float = x * 2.0 + 1.0
                y = y - 0.5
                var n is int = 7
                y = y + n
                return y as int
            endverb
    endclass
