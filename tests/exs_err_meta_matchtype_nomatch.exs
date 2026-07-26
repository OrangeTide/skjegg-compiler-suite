// exs_err_meta_matchtype_nomatch.exs : a meta `match` that matches no arm and
// has no `otherwise` is an error at the match (typed-macros.md D3). The base
// `match` statement is a no-op in that case, but a macro that quietly emits
// nothing surfaces much later as "produced no form", naming the wrong thing.
type
    record Vec3
        x is int
    endrecord

macro describe(v)
    match typeof(v)
        when int then quote "a number"
        when str then quote "a str"
    endmatch
endmacro

type
    class C
        public
            verb main(player is obj) returns int
                var a is Vec3 = Vec3(1)
                player.tell(describe(a))
                return 0
            endverb
    endclass
