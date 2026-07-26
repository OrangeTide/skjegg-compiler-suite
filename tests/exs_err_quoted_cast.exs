// exs_err_quoted_cast.exs : a type written inside a quote survives expansion.
// form_build clears an inferred type so a spliced form re-checks at the call
// site, but a cast's type is written, not inferred; clearing it made a quoted
// `as float` a cast to `any`, which assigns to anything. This pins the fix:
// the cast is still a cast to float, so a str target is rejected.
macro tofloat(v)
    quasi ${v} as float
endmacro

type
    class C
        public
            verb main(player is obj) returns int
                var d is str = tofloat(3)
                player.tell("d ${d}")
                return 0
            endverb
    endclass
