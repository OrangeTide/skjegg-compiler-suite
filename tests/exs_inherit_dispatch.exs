type
    class Base
        private
            hp is int = 0
        public
            verb heal(amount is int)
                hp = hp + amount
            endverb
    endclass
    class Hero is Base
        public
            verb main() returns int
                self.heal(20)
                self.heal(22)
                return hp
            endverb
    endclass
