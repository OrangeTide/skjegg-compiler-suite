// in a source body `return` loses its meaning: `yield` produces and
// `fail` (or the body's end) exhausts
type
    class C
        private
            func f() returns source of int
                yield 1
                return
            endfunc
        public
            verb main()
            endverb
    endclass
