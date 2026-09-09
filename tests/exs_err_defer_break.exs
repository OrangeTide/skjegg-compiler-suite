// break may not leave a deferred statement (a loop inside one is fine)
type
    class C
        private
            func f() returns int
                var t = 0
                for i in 1 to 3 do
                    defer break
                    t = t + i
                endfor
                return t
            endfunc
        public
            verb main()
            endverb
    endclass
