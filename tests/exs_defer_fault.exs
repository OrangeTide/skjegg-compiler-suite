// exs_defer_fault.exs : defer.md D4 - a hard fault aborts the turn and
// never runs the pending defers ("cleanup" must not print); the fault
// report goes to stderr and the exit code is 70.
type
    class C
        public
            verb main(player is obj) returns int
                var xs = [1 2 3]
                defer player.tell("cleanup")
                player.tell("before")
                player.tell("${xs[9]}")
                return 0
            endverb
    endclass
