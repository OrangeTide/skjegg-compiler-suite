// exs_source_yield.exs : sources.md continuation half - a func returning
// `source of T` suspends its body; `yield` produces one value per pump,
// `next` is the built-in pump, `for` sugars over it, and a wrapper source
// can pump another (each body runs on its own private stack).
type
    class C
        private
            func countdown(n is int) returns source of int
                while n > 0 do
                    yield n
                    n = n - 1
                endwhile
            endfunc
            func evens(s is source of int) returns source of int
                while var v = next(s) do
                    if v % 2 = 0 then yield v endif
                endwhile
            endfunc
            func words() returns source of str
                yield "alpha"
                yield "beta"
            endfunc
        public
            verb main(player is obj) returns int
                var s = countdown(3)
                for i in s do
                    player.tell("y ${i}")
                endfor
                for i in s do player.tell("dead ${i}") endfor
                player.tell("drained")

                var w = evens(countdown(6))     // a wrapper source
                while var v = next(w) do
                    player.tell("even ${v}")
                endwhile

                var t = countdown(4)
                if var x = next(t) then player.tell("first ${x}") endif
                for i in t do                   // picks up where t left off
                    if i = 2 then break endif
                    player.tell("rest ${i}")
                endfor

                for wrd in words() do player.tell(wrd) endfor
                return 0
            endverb
    endclass
