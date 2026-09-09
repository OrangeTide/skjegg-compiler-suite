// a record iterates only as a cursor: without a `next` func in the class
// the for loop is a teaching error naming the required signature
type
    record Point
        x is int
    endrecord
    class C
        public
            verb main()
                var p is Point = Point(1)
                for i in p do
                    var x = i
                endfor
            endverb
    endclass
