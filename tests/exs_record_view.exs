// exs_record_view.exs : a field-restricted by-reference parameter,
// `p is Point with (x, y)` (record-slicing.md D4). It is a `shared`-style view
// (no copy) whose body may access only the named fields, checked at compile
// time. A plain record parameter, by contrast, is copied (value semantics), so
// mutating it does not touch the caller. A whole record or a slice may be
// passed to a view.
type
    record Box
        w is int
        h is int
        tag is int
    endrecord
    class C
        private
            // a view restricted to w and h: it may read and write those,
            // and mutates the caller's record in place (no copy)
            func grow(b is Box with (w, h), by is int)
                b.w = b.w + by
                b.h = b.h + by
            endfunc

            // a plain record parameter is a value copy
            func spoil(b is Box)
                b.w = 999
                b.tag = 999
            endfunc

        public
            verb main(player is obj) returns int
                var theBox is Box = Box(10, 20, 1)
                grow(theBox, 5)                         // the view writes through
                player.tell("view ${theBox.w} ${theBox.h} ${theBox.tag}")   // 15 25 1

                var keep is Box = Box(3, 4, 7)
                spoil(keep)                          // the copy is discarded
                player.tell("copy ${keep.w} ${keep.tag}")          // 3 7

                // a slice may be passed to a view too
                grow(theBox with (w, h), 100)
                player.tell("slice ${theBox.w} ${theBox.h}")             // 115 125
                return 0
            endverb
    endclass
