// exs_list_record.exs : a list of record values (list<Point>). A record is a
// one-word pointer, so a record list reuses the word-element list machinery.
// Value semantics hold: a record entering the list is copied, and a record
// read out and bound is copied, so neither aliases the other. Membership
// compares elements by value, not by pointer identity.
type
    record Point
        x is int
        y is int
    endrecord
    class C
        private
            func showall(xs is list<Point>, player is obj)
                for p in xs do
                    player.tell("  ${p.x},${p.y}")
                endfor
            endfunc
        public
            verb main(player is obj) returns int
                var xs is list<Point> = [Point(1, 2) Point(3, 4)]
                player.tell("len ${length(xs)} p1 ${xs[1].x},${xs[1].y}")  // 2 / 1,2

                // build with the composition ops
                xs = prepend(xs, Point(0, 0))
                xs = append(xs, Point(5, 6))
                xs = insert(xs, 2, Point(9, 9))
                showall(xs, player)                   // 0,0 9,9 1,2 3,4 5,6

                var ys is list<Point> = reverse(xs)
                player.tell("ends ${first(ys).x} ${last(ys).x}")   // 5 0
                player.tell("rest ${rest(xs)[1].x}")               // 9
                var ws is list<Point> = xs + [Point(7, 7)]
                player.tell("cat ${length(ws)} ${last(ws).x}")     // 6 7
                player.tell("setdel ${set(xs, 1, Point(100, 0))[1].x} ${delete(xs, 1)[1].x}")  // 100 9

                // value semantics: a copy on insert, a copy on read
                var p is Point = Point(1, 2)
                var zs is list<Point> = append([], p)
                p.x = 99                              // mutate the source
                var q is Point = zs[1]                // read a copy out
                q.y = 88                              // mutate the copy
                player.tell("iso ${zs[1].x} ${zs[1].y}")           // 1 2 (both isolated)

                // membership compares by value, not pointer identity
                player.tell("mem ${Point(1, 2) in xs} ${Point(8, 8) in xs}")  // true false
                return 0
            endverb
    endclass
