const
    INC is int = 7
type
    class C
        private
            n is int = 0
        public
            verb bump()
                n = n + INC
            endverb
            verb main() returns int
                bump()
                bump()
                bump()
                return n
            endverb
    endclass
