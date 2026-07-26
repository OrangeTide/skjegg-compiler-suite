type
    class Entity
        private
            hp is int = 30
    endclass

    class Goblin is Entity
        private
            bonus is int = 12
        public
            verb main() returns int
                self.hp = self.hp + bonus
                return self.hp
            endverb
    endclass
