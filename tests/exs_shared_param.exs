// exs_shared_param.exs : `shared`, a by-reference parameter mode
// (shared-params.md). A `shared` parameter is Pascal's `var`: the func reads
// and writes the caller's own place through its address, not a copy. It is
// func-only (D5), the argument must be an lvalue (a local, a self field, or a
// forwarded shared param; D3/D4), and it is call-scoped so it never escapes
// (D7). The address is passed via IR_ADL (a local slot), IR_LEA (a self-field
// global), or the held address (a forwarded shared param).
type
    class C
        private
            score is int = 0

            func bump(shared n is int, by is int)
                n = n + by                      // writes through the caller's place
            endfunc

            func swap(shared a is int, shared b is int)
                var t is int = a
                a = b
                b = t
            endfunc

            func twice(shared n is int)         // forwards its shared parameter
                bump(n, 100)
                bump(n, 100)
            endfunc

        public
            verb main(player is obj) returns int
                var x is int = 10
                bump(x, 5)                       // a local by reference
                player.tell("x ${x}")            // 15

                var p is int = 1
                var q is int = 2
                swap(p, q)                       // two shared params
                player.tell("swap ${p} ${q}")    // 2 1

                bump(self.score, 7)              // a self field by reference
                player.tell("score ${self.score}")   // 7

                var k is int = 1
                twice(k)                         // a forwarded shared param
                player.tell("k ${k}")            // 201
                return 0
            endverb
    endclass
