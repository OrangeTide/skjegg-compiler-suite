// exs_comprehension.exs : a list comprehension, the one sugar of
// function-values.md (D5). `[HEAD for x in SOURCE (if COND)?]` lowers to a
// `for` loop building a list, so map and filter read plainly without a
// function value. This slice covers a single binder over a list or str
// source, with an optional filter; multiple `for` clauses and a range source
// are the deferred follow-ons.
type
    record Person
        name is str
        age is int
    endrecord

    class C
        public
            verb main(player is obj) returns int
                var xs is list<int> = [1 2 3 4 5 6]

                // map: each element transformed
                var doubled is list<int> = [x * 2 for x in xs]
                player.tell("doubled ${doubled[1]} ${doubled[6]}")     // 2 12

                // filter: a subset kept
                var evens is list<int> = [x for x in xs if x % 2 = 0]
                player.tell("evens ${length(evens)}: ${evens[1]} ${evens[3]}")

                // map and filter together
                var bigd is list<int> = [x * 2 for x in xs if x > 3]
                player.tell("bigd ${length(bigd)}: ${bigd[1]}")        // 3: 8

                // everything filtered out is the empty list
                var none is list<int> = [x for x in xs if x > 100]
                player.tell("none ${length(none)}")                    // 0

                // a record source, filtered and mapped by field (the headline)
                var people is list<Person> = [Person("ada", 36) Person("kai", 12) Person("mo", 40)]
                var adults is list<Person> = [p for p in people if p.age >= 18]
                player.tell("adults ${length(adults)}")                // 2
                var names is list<str> = [p.name for p in people]
                player.tell("names ${names[1]} ${names[3]}")           // ada mo

                // a str source iterates code points
                var cs is list<str> = ["${c}!" for c in "hi"]
                player.tell("cs ${cs[1]}${cs[2]}")                     // h!i!

                // a range source, with and without a filter
                var sq is list<int> = [i * i for i in 1 to 5]
                player.tell("sq ${sq[1]} ${sq[5]}")                    // 1 25
                var odds is list<int> = [i for i in 1 to 10 if i % 2 = 1]
                player.tell("odds ${length(odds)}: ${odds[5]}")        // 5: 9

                // a nested-head comprehension is a list of lists (the head is
                // any expression, so this falls out for free)
                var rows is list<int> = [1 2 3]
                var grid is list<list<int>> = [[r * c for c in rows] for r in rows]
                player.tell("grid ${length(grid)}")                    // 3

                // a plain data literal is unaffected (no `for`, no comprehension)
                var plain is list<int> = [7 8 9]
                player.tell("plain ${plain[2]}")                       // 8
                return 0
            endverb
    endclass
