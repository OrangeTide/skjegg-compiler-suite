// exs_err_comp_multifor.exs : a comprehension takes one `for` clause for now
// (function-values.md D5). Nested iteration (a second `for`) is a deferred
// follow-on, reported so the surface does not silently misparse.
type
    class C
        public
            verb main(player is obj) returns int
                var xs is list<int> = [1 2]
                var ys is list<int> = [3 4]
                var g is list<int> = [x + y for x in xs for y in ys]
                return 0
            endverb
    endclass
