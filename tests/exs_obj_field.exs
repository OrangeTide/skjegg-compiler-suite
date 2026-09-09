// exs_obj_field.exs : an object field holds another actor, so an object graph
// is expressible. An `obj`, class, or interface field is a 32-bit handle (nil
// is 0), lowered as one plain word with reference semantics: storing it copies
// the handle, not the object, and reading it yields the same actor.
//
// This exercises: a class-typed field read and re-sent through
// (`self.home.name()`), an obj field defaulting to nil and tested for it, a
// field reassigned to a different actor, two instances staying independent,
// and a back-reference (a Room holding its occupant) so the graph has a cycle.
type
    class Room
        private
            label is str = "nowhere"
            occupant is obj              // defaults to nil
        public
            verb name() returns str
                return self.label
            endverb
            verb rename(s is str)
                self.label = s
            endverb
            verb enter(who is obj)
                self.occupant = who
            endverb
            verb empty() returns bool
                return self.occupant = nil
            endverb
    endclass

    class Hero
        private
            home is Room                 // a class-typed field, defaults to nil
        public
            verb settle(r is Room)
                self.home = r
                self.home.enter(self)    // the room points back at the hero
            endverb
            verb whereAmI() returns str
                return self.home.name()  // read the field, send through it
            endverb
            verb moveTo(r is Room)
                self.home = r            // reassign to a different actor
            endverb
    endclass

    class C
        public
            verb main(player is obj) returns int
                var hall is Room = spawn(Room)
                var cave is Room = spawn(Room)
                hall.rename("the hall")
                cave.rename("the cave")

                var h is Hero = spawn(Hero)
                player.tell(hall.empty() then "hall empty" else "hall full")

                h.settle(hall)
                player.tell(h.whereAmI())                          // the hall
                player.tell(hall.empty() then "hall empty" else "hall full")

                h.moveTo(cave)
                player.tell(h.whereAmI())                          // the cave

                // a second hero is independent: it has its own home field
                var g is Hero = spawn(Hero)
                g.settle(hall)
                player.tell(g.whereAmI())                          // the hall
                player.tell(h.whereAmI())                          // still the cave
                return 0
            endverb
    endclass
