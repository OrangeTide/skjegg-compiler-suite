type
    class Robot
        private
            energy is int = 0
            func doubled() returns int
                return energy * 2
            endfunc
        public
            verb charge(amount is int)
                energy = energy + amount
            endverb
            verb main() returns int
                self.charge(10)
                self.charge(11)
                return doubled()
            endverb
    endclass
