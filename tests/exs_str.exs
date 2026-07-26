type
    class C
        public
            verb main() returns int
                var a is str = "ab"
                var b is str = "cd"
                var s is str = a + b
                var r is int = 0
                if s = "abcd" then
                    r = r + 1
                endif
                if s <> "abce" then
                    r = r + 2
                endif
                if a = b then
                    r = r + 4
                endif
                if a + b + "ef" = "abcdef" then
                    r = r + 8
                endif
                return r
            endverb
    endclass
