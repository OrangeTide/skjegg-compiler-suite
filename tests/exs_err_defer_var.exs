// a defer cannot declare a variable: the teardown runs at block exit,
// so nothing is left to hold it
type
    class C
        private
            func f() returns int
                defer var x = 1
                return 0
            endfunc
        public
            verb main()
            endverb
    endclass
