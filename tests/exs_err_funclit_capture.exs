// exs_err_funclit_capture.exs : a func value captures nothing
// (function-values.md D4). A reference to an enclosing local is rejected;
// an escaping captured closure waits on the memory-management pass. The
// actor-safe move is to pass what the func needs as a parameter.
type
    class C
        public
            verb main(player is obj) returns int
                var k is int = 100
                var f = func(x is int) returns int
                            return x + k
                        endfunc
                player.tell("${f(1)}")
                return 0
            endverb
    endclass
