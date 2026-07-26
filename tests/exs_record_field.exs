// exs_record_field.exs : records.md - a record-typed class field (a pointer
// word to the record's arena block), and a function that returns a record.
// Both preserve value semantics: assigning a record copies it.
type
    record Point
        x is int
        y is int
    endrecord
    class C
        private
            home is Point
            func mk(a is int, b is int) returns Point
                return Point(a, b)
            endfunc
        public
            verb main(player is obj) returns int
                self.home = mk(1, 2)             // a returned record into a field
                player.tell("home ${self.home.x} ${self.home.y}")   // 1 2

                var g is Point = self.home        // copy the field into a local
                g.x = 99                          // mutate the copy
                player.tell("copy ${self.home.x} ${g.x}")   // 1 99 (value copy)

                self.home.x = self.home.x + 10    // read-modify-write the field
                player.tell("rmw ${self.home.x} ${self.home.y}")   // 11 2

                var other is Point = Point(5, 6)
                self.home = other                 // copy a local into the field
                other.y = 77                      // mutate the source
                player.tell("indep ${self.home.y} ${other.y}")   // 6 77
                return 0
            endverb
    endclass
