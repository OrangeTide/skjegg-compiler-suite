// exs_record_ufcs.exs : records.md D4 - UFCS. p.f(args) is f(p, args) when f
// is a func (not a field of the record) whose first parameter is the record.
type
    record Point
        x is int
        y is int
    endrecord
    class C
        private
            func sumxy(p is Point) returns int
                return p.x + p.y
            endfunc
            func scaled(p is Point, by is int) returns int
                return (p.x + p.y) * by
            endfunc
        public
            verb main(player is obj) returns int
                var p is Point = Point(3, 4)
                player.tell("sum ${p.sumxy()}")         // 7  -> sumxy(p)
                player.tell("scaled ${p.scaled(10)}")   // 70 -> scaled(p, 10)
                return 0
            endverb
    endclass
