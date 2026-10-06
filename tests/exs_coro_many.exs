// exs_coro_many.exs : V.F coroutine stress - many simultaneously live
// coroutines pumped round-robin. Six independent sources are each held in a
// var and advanced one step at a time in turn, so all six are suspended at
// once and every pump resumes a different private stack than the last. This
// is the many-live case the single-chain deep test does not cover: the live
// coroutines are peers, not nested, so their stacks are switched among with
// no enclosing frame holding them.
type
    class C
        private
            func count(lo is int, hi is int) returns source of int
                var i = lo
                while i <= hi do
                    yield i
                    i = i + 1
                endwhile
            endfunc
        public
            verb main(player is obj) returns int
                var a = count(1, 3)
                var b = count(10, 12)
                var c = count(20, 22)
                var d = count(30, 31)
                var e = count(40, 42)
                var f = count(50, 50)
                // round-robin: pump each in turn until all are drained
                var live = 6
                while live > 0 do
                    live = 6
                    if var va = next(a) then
                        player.tell("a ${va}")
                    else
                        live = live - 1
                    endif
                    if var vb = next(b) then
                        player.tell("b ${vb}")
                    else
                        live = live - 1
                    endif
                    if var vc = next(c) then
                        player.tell("c ${vc}")
                    else
                        live = live - 1
                    endif
                    if var vd = next(d) then
                        player.tell("d ${vd}")
                    else
                        live = live - 1
                    endif
                    if var ve = next(e) then
                        player.tell("e ${ve}")
                    else
                        live = live - 1
                    endif
                    if var vf = next(f) then
                        player.tell("f ${vf}")
                    else
                        live = live - 1
                    endif
                endwhile
                player.tell("all drained")
                return 0
            endverb
    endclass
