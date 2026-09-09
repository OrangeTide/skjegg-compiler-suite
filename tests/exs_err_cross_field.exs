// exs_err_cross_field.exs : a field of another actor is not directly
// readable (data-model.md, the actor boundary). `self.field` is the one
// exception; foreign state is reached by sending a verb. Records are values,
// not actors, so `p.x` on a record stays fine.
type
    class Room
        private
            label is str = "hall"
    endclass
    class Hero
        private
            home is Room
        public
            verb peek(other is Hero) returns str
                return other.home.name()
            endverb
    endclass
    class C
        public
            verb main(player is obj) returns int
                return 0
            endverb
    endclass
