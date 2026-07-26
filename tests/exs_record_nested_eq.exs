// exs_record_nested_eq.exs : records.md - value comparison (`=` / `<>`) over
// records that contain nested records. Records nest flat, so `__exc_rec_eq`
// compares the flattened words; a str field at any nesting depth compares by
// content (its flat position is flagged in the str bitmask).
type
    record Point
        x is int
        y is int
    endrecord
    record Line
        a is Point
        b is Point
    endrecord
    record Tagged
        name is str
        at is Point
    endrecord
    class C
        public
            verb main(player is obj) returns int
                var l1 is Line = Line(Point(1, 2), Point(3, 4))
                var l2 is Line = Line(Point(1, 2), Point(3, 4))
                var l3 is Line = Line(Point(1, 2), Point(9, 4))
                player.tell("eq ${l1 = l2} ne ${l1 = l3} neop ${l1 <> l3}")  // true false true
                l3.b.x = 3
                player.tell("afterfix ${l1 = l3}")   // true

                // a str field inside a nested record still compares by content
                var t1 is Tagged = Tagged("orc", Point(5, 6))
                var t2 is Tagged = Tagged("orc", Point(5, 6))
                var t3 is Tagged = Tagged("orc", Point(5, 7))
                var t4 is Tagged = Tagged("elf", Point(5, 6))
                player.tell("teq ${t1 = t2} tne ${t1 = t3} tstr ${t1 = t4}")  // true false false
                return 0
            endverb
    endclass
