type
    class C
        public
            verb main() returns int
                var xs is list<int> = [10 20 30]
                var names is list<str> = ["red" "green" "blue"]
                var r is int = 0
                if 20 in xs then
                    r = r + 1
                endif
                if 99 in xs then
                    r = r + 2
                endif
                if "green" in names then
                    r = r + 4
                endif
                if "pink" in names then
                    r = r + 8
                endif
                if "ll" in "hello" then
                    r = r + 16
                endif
                if "xyz" in "hello" then
                    r = r + 32
                endif
                var v is int = 30
                if v in xs then
                    r = r + 64
                endif
                return r
            endverb
    endclass
