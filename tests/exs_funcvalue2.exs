// exs_funcvalue2.exs : func value cases the first review surfaced. A lambda
// may call a sibling FUNC (a static helper, no self): it lowers as its own
// top-level function, so it must mangle the call against its defining class,
// not whichever class was lowered last. A non-capturing func may also be
// returned, since it is a free value (function-values.md D4). Zebra is here
// only to be lowered after C, which is what exposed the mangling bug.
type
    class C
        private
            func triple(n is int) returns int
                return n * 3
            endfunc
            // a factory: returns a non-capturing func value (a free value)
            func makeAdder() returns func(int) returns int
                return func(x is int) returns int
                           return x + 1
                       endfunc
            endfunc
        public
            verb main(player is obj) returns int
                var f = func(x is int) returns int
                            return triple(x) + 1     // a sibling func call
                        endfunc
                player.tell("sib ${f(10)}")          // 31
                var add1 = makeAdder()
                player.tell("made ${add1(41)}")      // 42
                return 0
            endverb
    endclass

    class Zebra
        public
            verb ping() returns int
                return 0
            endverb
    endclass
