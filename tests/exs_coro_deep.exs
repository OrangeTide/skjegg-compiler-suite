// exs_coro_deep.exs : V.F coroutine stress - deep wrapper-source nesting.
// Each `add1` wrapper pumps the source below it and yields value+1, so a chain
// of twelve wrappers over one countdown is thirteen coroutines simultaneously
// live, each suspended on its own private stack. Draining the chain switches
// through every one of those stacks on every pump, which is the save_ctx /
// restore_ctx path under its heaviest nesting.
type
    class C
        private
            func countdown(n is int) returns source of int
                while n > 0 do
                    yield n
                    n = n - 1
                endwhile
            endfunc
            func add1(s is source of int) returns source of int
                while var v = next(s) do
                    yield v + 1
                endwhile
            endfunc
        public
            verb main(player is obj) returns int
                // twelve wrappers: each yielded value is the countdown value
                // plus twelve
                var chain = add1(add1(add1(add1(add1(add1(
                            add1(add1(add1(add1(add1(add1(
                                countdown(3)))))))))))))
                for v in chain do
                    player.tell("deep ${v}")
                endfor
                player.tell("drained")
                return 0
            endverb
    endclass
