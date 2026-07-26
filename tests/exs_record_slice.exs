// exs_record_slice.exs : record-slicing.md - `p with (x, y)` names a subset
// of a record's fields, both to copy them in place from a source (D3) and to
// compare only those fields (D3). No runtime form; the compiler unrolls it.
type
    record Point3
        x is int
        y is int
        z is int
    endrecord
    record Tagged
        name is str
        rank is int
    endrecord
    class C
        public
            verb main(player is obj) returns int
                var a is Point3 = Point3(1, 2, 3)
                var b is Point3 = Point3(10, 20, 30)
                // copy just x and y from b into a; z is untouched
                a with (x, y) = b
                player.tell("a ${a.x} ${a.y} ${a.z}")       // 10 20 3

                // compare only the named fields
                var c is Point3 = Point3(10, 20, 99)
                var xyEq is bool = a with (x, y) = c
                player.tell("xyEq ${xyEq}")                  // true
                var allEq is bool = a = c
                player.tell("allEq ${allEq}")                // false
                var zNe is bool = a with (z) <> c
                player.tell("zNe ${zNe}")                    // true

                // a slice on both sides of a comparison
                var same is bool = a with (x, y) = b with (x, y)
                player.tell("same ${same}")                  // true

                // str fields compare by content
                var t1 is Tagged = Tagged(name: "orc", rank: 3)
                var t2 is Tagged = Tagged(name: "orc", rank: 9)
                player.tell("nameEq ${t1 with (name) = t2}") // true
                return 0
            endverb
    endclass
