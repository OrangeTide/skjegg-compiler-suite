// exs_record_eq.exs : records.md D5 - value equality. Two records are equal
// when all fields are (`=`, equality.md), so distinct copies with equal values
// compare equal; `<>` is not-equal.
type
    record Point
        x is int
        y is int
    endrecord
    record Named
        name is str
        rank is int
    endrecord
    class C
        public
            verb main(player is obj) returns int
                var a is Point = Point(3, 4)
                var b is Point = Point(3, 4)       // equal value, distinct copy
                var c is Point = Point(3, 9)
                player.tell("ab ${a = b}")         // true
                player.tell("ac ${a = c}")         // false
                player.tell("ne ${a <> c}")         // true
                var p is Named = Named("Frodo", 1)
                var q is Named = Named("Frodo", 1)  // str field equal by content
                var w is Named = Named("Sam", 1)
                player.tell("pq ${p = q}")         // true
                player.tell("pw ${p = w}")         // false
                return 0
            endverb
    endclass
