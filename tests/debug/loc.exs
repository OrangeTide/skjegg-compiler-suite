// Fixture for test-exc-loc: debug-info line markers (-g) on the Excelsior path.
// Each statement sits on its own line so skj-exc emits one IR_LOC per line and
// the ColdFire backend records a "| line N" comment.  The test only inspects
// the assembly, so the program need not do anything interesting.
type
    class C
        private
            func add(a is int, b is int) returns int
                var s is int = a + b
                return s
            endfunc
        public
            verb main(player is obj) returns int
                var x is int = add(2, 3)
                x = x + 1
                return x
            endverb
    endclass
