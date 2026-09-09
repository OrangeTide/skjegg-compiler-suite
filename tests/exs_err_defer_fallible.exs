// a deferred statement must be infallible or consume its own failure:
// an unwind has no consumer for one it raises
type
    class C
        private
            x is int
            func f(xs is list<int>) returns maybe int
                defer x = xs[9]
                return xs[1]
            endfunc
        public
            verb main()
            endverb
    endclass
