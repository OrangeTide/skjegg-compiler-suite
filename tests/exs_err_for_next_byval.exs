// a pump that takes its cursor by value could never advance it: the
// signature must be `next(shared c is R)`, and the checker says so
type
    record Counter
        cur is int
    endrecord
    class C
        private
            func next(c is Counter) returns maybe int
                return nothing
            endfunc
        public
            verb main()
                var c is Counter = Counter(0)
                for i in c do
                    var x = i
                endfor
            endverb
    endclass
