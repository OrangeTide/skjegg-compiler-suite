// exs_err_on_fail_infallible.exs : `on fail` is the statement consumer of a
// failure (fallible-consumers.md), so an action that cannot fail has nothing
// for it to catch; the message says to drop it.
type
    class C
        private
            func plain() returns int
                return 1
            endfunc
        public
            verb main(player is obj) returns int
                plain() on fail player.tell("no")
                return 0
            endverb
    endclass
