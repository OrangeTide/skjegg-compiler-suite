// exs_prelude.exs : the prelude combinators map / filter / sort / reduce
// (function-values.md D6/D7). Each takes a list and a func value and returns a
// fresh copy-on-write list; the source is untouched. They are compiler-known
// (a runtime helper calls the func value per element) but read as ordinary
// function calls, which is the point, not a baked `by` clause. They compose,
// and `map` may change the element type.
type
    class C
        public
            verb main(player is obj) returns int
                var xs is list<int> = [3 1 4 1 5 9 2 6]

                // map: transform each element
                var doubled = map(xs, func(x is int) returns int
                                          return x * 2
                                      endfunc)
                player.tell("map ${doubled[1]} ${doubled[8]}")       // 6 12

                // filter: keep a subset
                var evens = filter(xs, func(x is int) returns bool
                                           return x % 2 = 0
                                       endfunc)
                player.tell("filter ${length(evens)}")               // 3

                // sort: ascending by a comparator
                var up = sort(xs, func(a is int, b is int) returns bool
                                      return a < b
                                  endfunc)
                player.tell("sort ${up[1]} ${up[8]}")                // 1 9

                // map may change the element type (int -> str)
                var labels = map([1 2 3], func(x is int) returns str
                                              return "n${x}"
                                          endfunc)
                player.tell("labels ${labels[1]} ${labels[3]}")      // n1 n3

                // the combinators compose: filter then sort descending
                var desc = sort(filter(xs, func(x is int) returns bool
                                               return x > 3
                                           endfunc),
                                func(a is int, b is int) returns bool
                                    return a > b
                                endfunc)
                player.tell("desc ${desc[1]} ${length(desc)}")       // 9 4

                // reduce: fold to a single value, seeded by an initial value
                var total = reduce(xs, 0, func(acc is int, x is int) returns int
                                              return acc + x
                                          endfunc)
                player.tell("sum ${total}")                          // 31

                // reduce may fold to a different type (int list -> str)
                var joined = reduce([1 2 3], "", func(acc is str, x is int) returns str
                                                     return "${acc}${x}"
                                                 endfunc)
                player.tell("join ${joined}")                        // 123

                // the source is untouched (copy-on-write)
                player.tell("src ${xs[1]} ${length(xs)}")            // 3 8
                return 0
            endverb
    endclass
