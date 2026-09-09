// exs_walker_fixture.exs : the freeze/thaw walker test's class. No
// .expected/.exitcode: run-exc-tests only compile-checks it; the real
// run is `make test-exc-walker` (tests/exc_walker_test.c drives the
// compiled descriptor's field table under qemu).
type
    class Chest
        private
            hp    is int = 10
            label is str = "plain"
            open  is bool
            count is int
            stash is maybe int
            owner is obj                // a handle: skipped by freeze/thaw
        public
            verb poke()
                hp = hp + 1
            endverb
    endclass
