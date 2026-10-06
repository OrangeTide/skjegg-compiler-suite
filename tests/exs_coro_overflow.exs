// exs_coro_overflow.exs : V.F coroutine stress - a coroutine stack overrun
// must hit the guard page, not corrupt the arena. The source body calls an
// unbounded recursion (spin never returns, and the `+ 1` keeps it off any
// tail-call path), which grows the coroutine's private 4 KB stack downward
// into the unmapped guard page below it. On skj-run that is a "stack
// overflow" fault (exit 139, SIGSEGV), reported before any value is yielded.
// This test has no .expected/.exitcode: the guarded coroutine stack is a
// skj-run feature (qemu falls back to the arena with no guard), so it is
// driven only by `make test-exc-coro-overflow`, never the portable tiers.
type
    class C
        private
            func spin(n is int) returns int
                return 1 + spin(n + 1)
            endfunc
            func boom() returns source of int
                yield spin(0)
            endfunc
        public
            verb main(player is obj) returns int
                for v in boom() do
                    player.tell("got ${v}")
                endfor
                player.tell("unreachable")
                return 0
            endverb
    endclass
