// exs_err_meta_matchtype_kind.exs : a label of a different kind from the
// subject is a mistake worth naming (typed-macros.md D3), since a meta match
// dispatches over types and atoms alike and mixing them reads plausibly.
macro describe(v)
    match typeof(v)
        when int then quote "a number"
        when 7 then quote "seven"
    endmatch
endmacro

type
    class C
        public
            verb main(player is obj) returns int
                player.tell(describe("x"))
                return 0
            endverb
    endclass
