// exs_spawn_instances.exs : every object carries its own field segment, so
// two actors of one class never share storage. Before this, a field was a
// module global mangled by its declaring class, and two spawned Counters
// counted into the same word.
//
// Four things are pinned here: instances are independent, an inherited field
// keeps its offset in a subclass (the segment is laid out parent-first), each
// new object starts from its class's defaults, and a verb that sends away and
// comes back still sees its own fields (the send saves and restores the
// running actor).
type
    class Counter
        private
            n is int = 0
        public
            verb bump() returns int
                self.n = self.n + 1
                return self.n
            endverb
            verb get() returns int
                return self.n
            endverb
    endclass

    class Animal
        private
            hp is int = 10
            name is str = "beast"
        public
            verb hurt(by is int)
                self.hp = self.hp - by
            endverb
            verb describe() returns str
                return self.name + " ${self.hp}"
            endverb
    endclass

    class Orc is Animal
        private
            rage is int = 3
        public
            verb enrage()
                self.rage = self.rage + 1
                self.hp = self.hp + 5           // an inherited field
            endverb
            verb describe() returns str         // overrides
                return "orc ${self.hp}/${self.rage}"
            endverb
    endclass

    class Boss
        private
            tally is int = 100
        public
            verb run(player is obj, c is Counter) returns int
                self.tally = self.tally + 1
                c.bump()                        // sends away, changing self
                self.tally = self.tally + 1     // still this Boss's tally
                player.tell("tally ${self.tally}")
                return self.tally
            endverb
    endclass

    class C
        public
            verb main(player is obj) returns int
                var a is Counter = spawn(Counter)
                var b is Counter = spawn(Counter)
                a.bump()
                a.bump()
                b.bump()
                player.tell("a ${a.get()} b ${b.get()}")     // a 2 b 1

                var beast is Animal = spawn(Animal)
                var o is Orc = spawn(Orc)
                var o2 is Orc = spawn(Orc)
                beast.hurt(3)
                o.enrage()
                o.hurt(1)
                player.tell(beast.describe())                // beast 7
                player.tell(o.describe())                    // orc 14/4
                player.tell(o2.describe())                   // orc 10/3

                var boss is Boss = spawn(Boss)
                boss.run(player, a)                          // tally 102
                player.tell("a ${a.get()}")                  // a 3
                return 0
            endverb
    endclass
