// exs_obj_record_field.exs : a record field of a class is laid out INLINE in
// the object's segment, exactly as a record nests in a record. One layout
// engine answers for both aggregates (agg_words / agg_woffset), so the same
// declaration means the same thing in either container.
//
// Two consequences over the old pointer-word form: a fresh object's record
// field is a zero record rather than a null pointer, so reading it before
// assigning is well defined, and assigning one blits into the segment instead
// of allocating a separate block.
type
    record Point
        x is int
        y is int
    endrecord

    record Span
        lo is Point
        hi is Point
    endrecord

    class Holder
        private
            p is Point
            s is Span
            tag is int = 7
        public
            verb setp(nx is int)
                self.p = Point(nx, 99)
            endverb
            verb setspan(a is int, b is int)
                self.s = Span(Point(a, a), Point(b, b))
            endverb
            verb probe() returns str
                return "${self.p.x}/${self.p.y} ${self.s.lo.x}-${self.s.hi.x} tag ${self.tag}"
            endverb
    endclass

    class C
        public
            verb main(player is obj) returns int
                var fresh is Holder = spawn(Holder)
                player.tell(fresh.probe())          // 0/0 0-0 tag 7

                var h1 is Holder = spawn(Holder)
                var h2 is Holder = spawn(Holder)
                h1.setp(1)
                h1.setspan(2, 3)
                h2.setp(4)
                player.tell(h1.probe())             // 1/99 2-3 tag 7
                player.tell(h2.probe())             // 4/99 0-0 tag 7
                return 0
            endverb
    endclass
