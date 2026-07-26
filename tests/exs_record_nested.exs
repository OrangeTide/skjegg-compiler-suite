// exs_record_nested.exs : records.md - records nest flat. A record field
// whose type is a record is laid out inline inside the parent's block (no
// pointer indirection); reading it yields the interior pointer, assigning or
// constructing it blits the sub-record's words. Value semantics still hold,
// so a copy taken from a nested value is independent of the original.
type
    record Point
        x is int
        y is int
    endrecord
    record Line
        a is Point
        b is Point
    endrecord
    record Poly
        edge is Line
        sides is int
    endrecord
    class C
        public
            verb main(player is obj) returns int
                var ln is Line = Line(Point(1, 2), Point(3, 4))
                player.tell("read ${ln.a.x} ${ln.a.y} ${ln.b.x} ${ln.b.y}")  // 1 2 3 4

                ln.a.x = 99                     // write a nested scalar
                player.tell("write ${ln.a.x} ${ln.b.x}")   // 99 3 (b untouched)

                var p is Point = ln.b           // copy a nested value out
                p.y = 77
                player.tell("copyout ${ln.b.y} ${p.y}")    // 4 77 (independent)

                ln.a = Point(5, 6)              // assign a whole nested record
                player.tell("whole ${ln.a.x} ${ln.a.y}")   // 5 6

                var l2 is Line = ln             // deep copy a nesting-containing record
                l2.a.x = 88
                player.tell("deepcopy ${ln.a.x} ${l2.a.x}")   // 5 88 (independent)

                var pg is Poly = Poly(Line(Point(10, 20), Point(30, 40)), 4)
                player.tell("threedeep ${pg.edge.a.x} ${pg.edge.b.y} ${pg.sides}")  // 10 40 4
                return 0
            endverb
    endclass
