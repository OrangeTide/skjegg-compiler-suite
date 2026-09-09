// exs_funcvalue.exs : the func value core (function-values.md D1). An
// anonymous `func(params) returns T ... endfunc` is a value, a code pointer
// at rest, bound to a local, passed as an argument, and called through (an
// indirect call). A func-typed parameter `f is func(int) returns bool` is how
// a higher-order func receives one. A lambda captures nothing, so it may name
// module consts but not enclosing locals or self.
const
    BONUS = 100
type
    class C
        private
            // a general "apply a predicate and count matches" helper —
            // the shape a filter combinator would take
            func countIf(xs is list<int>, keep is func(int) returns bool) returns int
                var c is int = 0
                for x in xs do
                    if keep(x) then c = c + 1 endif
                endfor
                return c
            endfunc
        public
            verb main(player is obj) returns int
                // a lambda may name a module const (not a capture)
                var addBonus = func(x is int) returns int
                                   return x + BONUS
                               endfunc
                player.tell("bonus ${addBonus(5)}")            // 105

                // two-param lambda
                var add = func(a is int, b is int) returns int
                              return a + b
                          endfunc
                player.tell("add ${add(3, 4)}")                // 7

                // pass a predicate lambda to a higher-order func
                var xs is list<int> = [1 2 3 4 5 6]
                var evens = countIf(xs, func(x is int) returns bool
                                            return x % 2 = 0
                                        endfunc)
                player.tell("evens ${evens}")                  // 3
                return 0
            endverb
    endclass
