// the unwind is already on its way out: a deferred statement may not return
type
    class C
        private
            func f() returns int
                defer return 1
                return 0
            endfunc
        public
            verb main()
            endverb
    endclass
