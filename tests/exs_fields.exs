type
    class Counter
        private
            count is int  = 10
            step  is int  = 5
            armed is bool = true
        public
            verb main() returns int
                count = count + step
                self.count = self.count + step
                if armed then
                    self.count = self.count - 1
                endif
                return count
            endverb
    endclass
