// exs_err_sort_cmp.exs : sort's comparator must return bool (whether its
// first argument sorts ahead of its second); a non-bool result is an error
// (function-values.md D6).
type
    class C
        public
            verb main(player is obj) returns int
                var xs is list<int> = [3 1 2]
                var s = sort(xs, func(a is int, b is int) returns int
                                     return a - b
                                 endfunc)
                player.tell("${s[1]}")
                return 0
            endverb
    endclass
