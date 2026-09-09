// `yield` belongs to a source body only; a plain func returns
type
    class C
        private
            func f() returns int
                yield 1
                return 0
            endfunc
        public
            verb main()
            endverb
    endclass
