// the source constructor stores its arguments as words in the source
// struct, so an 8-byte float parameter is rejected with a teaching error
// rather than silently truncated
type
    class C
        private
            func f(x is float) returns source of int
                yield 1
            endfunc
        public
            verb main()
            endverb
    endclass
