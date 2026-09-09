// exs_source_cursor.exs : sources.md cursor half - a record with a
// `next(shared c is R) returns maybe T` func is a cursor source: `while var`
// is the manual pump, `for x in E` its sugar over a hidden cursor copy.
type
    record Counter
        cur is int
        hi  is int
    endrecord
    class C
        private
            func next(shared c is Counter) returns maybe int
                if c.cur >= c.hi then return nothing endif
                c.cur = c.cur + 1
                return c.cur
            endfunc
        public
            verb main(player is obj) returns int
                var c is Counter = Counter(0, 3)
                while var i = next(c) do            // the manual pump
                    player.tell("pump ${i}")
                endwhile
                player.tell("after ${c.cur}")       // 3: pumped in place

                var f is Counter = Counter(0, 3)
                for i in f do
                    player.tell("for ${i}")
                endfor
                player.tell("undisturbed ${f.cur}") // 0: the loop copies

                for i in f do player.tell("again ${i}") endfor

                for i in Counter(0, 5) do           // fresh ctor: no copy
                    if i = 2 then continue endif    // continue re-pumps
                    if i = 4 then break endif
                    player.tell("cb ${i}")
                endfor
                return 0
            endverb
    endclass
