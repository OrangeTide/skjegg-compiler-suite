// exs_record.exs : records.md - record literals (positional and named) and
// named function arguments, both using the `name: value` form.
type
    record Point
        x is int
        y is int
    endrecord
    record StatBlock
        might is int
        hp    is int = 10
    endrecord
    class C
        private
            func mix(base is int, add is int) returns int
                return base + add
            endfunc
        public
            verb main(player is obj) returns int
                var p is Point = Point(3, 4)             // positional literal
                player.tell("x ${p.x} y ${p.y}")         // 3 4
                var q is Point = p
                q.x = 99
                player.tell("p ${p.x} q ${q.x}")          // 3 99
                p.x = p.x + 5
                player.tell("p2 ${p.x}")                  // 8
                var np is Point = Point(x: 1, y: 2)       // named literal
                var ro is Point = Point(y: 8, x: 7)       // order-independent
                player.tell("np ${np.x} ${np.y} ro ${ro.x} ${ro.y}")   // 1 2 7 8
                var s is StatBlock = StatBlock(might: 12)  // hp defaults
                player.tell("s ${s.might} ${s.hp}")       // 12 10
                player.tell("mix ${mix(add: 10, base: 5)}") // 15 (named, reordered)
                return 0
            endverb
    endclass
