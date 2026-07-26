// exs_err_dotdot.exs : the `..` range was removed; a range is written `lo to hi`
type
    class C
        public
            verb main(player is obj) returns int
                var xs is list<int> = [1 2 3]
                var ys is list<int> = xs[1 .. 2]
                return 0
            endverb
    endclass
