type
    class C
        private
            level is decimal = 1.5
        public
            verb bump()
                level = level + 1.0
            endverb
            verb main() returns int
                bump()
                bump()
                bump()
                return level as int
            endverb
    endclass
