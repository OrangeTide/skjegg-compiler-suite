// exs_defer.exs : defer.md - block-scoped teardown on every orderly exit:
// fall-through, return (value computed first), break/continue
// (per-iteration, reading current values), and fallible propagation; LIFO
// order; the on-fail form for a teardown that can itself fail.
type
    class C
        private
            log  is str = ""
            busy is bool
            val  is int
            func note(s is str)
                log = log + s + " "
            endfunc
            func work(xs is list<int>, i is int) returns maybe int
                busy = true
                defer busy = false
                defer note("w2")
                defer note("w1")            // LIFO: w1 runs before w2
                return xs[i] + 1            // may fail: defers still run
            endfunc
            func pre() returns int
                val = 5
                defer val = 99
                return val + 1              // 6: computed before the defer
            endfunc
            func counted() returns int
                var total = 0
                for i in 1 to 5 do
                    defer note("i${i}")     // per-iteration, current i
                    if i = 3 then continue endif
                    if i = 5 then break endif
                    total = total + i
                endfor
                return total
            endfunc
            func mayfail(ok is bool) can fail
                if not ok then fail endif
            endfunc
            func g(xs is list<int>, deep is bool) returns maybe int
                defer note("outer")
                if deep then
                    defer note("inner")
                    return xs[9]        // fails: inner then outer run,
                endif                   // then the failure leaves g
                return xs[1]
            endfunc
            func finale() returns int
                defer note("m2")
                defer mayfail(false) on fail note("caught")
                defer note("m1")
                return 7
            endfunc
        public
            verb main(player is obj) returns int
                var xs = [10 20 30]
                var a = work(xs, 2) otherwise -1
                player.tell("a ${a} busy ${busy then 1 else 0} log ${log}")
                log = ""
                var b = work(xs, 9) otherwise -1
                player.tell("b ${b} busy ${busy then 1 else 0} log ${log}")
                log = ""
                player.tell("pre ${pre()} val ${val}")
                log = ""
                player.tell("counted ${counted()} log ${log}")
                log = ""
                player.tell("finale ${finale()} log ${log}")
                log = ""
                var c = g(xs, true) otherwise -1
                player.tell("chain ${c} log ${log}")
                return 0
            endverb
    endclass
