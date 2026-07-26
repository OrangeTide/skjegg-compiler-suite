// exs_list_record_nested.exs : a list of records that themselves nest records
// (list<Line>, where Line holds two Points). The element copy and value
// membership walk the flattened record, so a nested field chain off an index
// (`ls[i].a.x`) and value membership both work.
type
    record Point
        x is int
        y is int
    endrecord
    record Line
        a is Point
        b is Point
    endrecord
    class C
        public
            verb main(player is obj) returns int
                var ls is list<Line> = [Line(Point(1, 2), Point(3, 4)) Line(Point(5, 6), Point(7, 8))]
                player.tell("chain ${ls[2].a.x} ${ls[2].b.y}")   // 5 8
                player.tell("mem ${Line(Point(1, 2), Point(3, 4)) in ls}")  // true
                player.tell("nomem ${Line(Point(1, 2), Point(9, 9)) in ls}")  // false
                for ln in ls do
                    player.tell("  ${ln.a.x}-${ln.b.y}")         // 1-4 then 5-8
                endfor
                return 0
            endverb
    endclass
