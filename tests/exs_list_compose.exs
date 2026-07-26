// exs_list_compose.exs : list-ops.md composition and decomposition. prepend,
// insert, reverse, and `+` concat return fresh copy-on-write lists; first/last
// are fallible point access (empty fails), rest is the all-but-first slice.
type
    class C
        public
            verb main(player is obj) returns int
                var xs is list<int> = [2 3 4]
                var a is list<int> = prepend(xs, 1)          // [1 2 3 4]
                player.tell("prepend ${length(a)} ${a[1]}")  // 4 1
                var b is list<int> = insert(xs, 2, 9)        // [2 9 3 4]
                player.tell("insert ${length(b)} ${b[2]}")   // 4 9
                var c is list<int> = reverse(xs)             // [4 3 2]
                player.tell("reverse ${c[1]} ${c[3]}")       // 4 2
                var d is list<int> = xs + [5 6]              // [2 3 4 5 6]
                player.tell("concat ${length(d)} ${d[5]}")   // 5 6
                player.tell("first ${first(xs)}")            // 2
                player.tell("last ${last(xs)}")              // 4
                var r is list<int> = rest(xs)                // [3 4]
                player.tell("rest ${length(r)} ${r[1]}")     // 2 3
                var empty is list<int> = []
                var f is int = first(empty) otherwise 0 - 1  // fails -> -1
                player.tell("empty ${f} ${length(rest(empty))}")  // -1 0
                return 0
            endverb
    endclass
