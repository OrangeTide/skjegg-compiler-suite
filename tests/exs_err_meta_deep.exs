// exs_err_meta_deep.exs : a macro that emits a call to itself never
// terminates, and the depth cap must report it (typed-macros.md D2). The cap
// has to wrap the re-check, not just the expansion: the recursion runs through
// the checker, so counting only expand_macro let this run the C stack out.
macro loop(v)
    quasi loop(${v})
endmacro

type
    class C
        public
            verb main(player is obj) returns int
                player.tell("n ${loop(1)}")
                return 0
            endverb
    endclass
