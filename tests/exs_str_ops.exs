type
    class C
        public
            verb main() returns int
                var s is str = "hello"
                var r is int = 0
                if "abc" < "abd" then
                    r = r + 1
                endif
                if "abc" >= "abc" then
                    r = r + 2
                endif
                if "abd" > "abc" then
                    r = r + 4
                endif
                if "abc" <= "ab" then
                    r = r + 8
                endif
                var c is str = s[1]      // fallible index; hoist then test
                if c = "h" then
                    r = r + 16
                endif
                if s[2 to 4] = "ell" then
                    r = r + 32
                endif
                if s[0 to 2] = "he" then
                    r = r + 64
                endif
                if s[3 to 99] = "llo" then
                    r = r + 128
                endif
                return r
            endverb
    endclass
