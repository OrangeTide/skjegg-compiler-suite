// exs_err_enum_exhaustive.exs : a match on an enum must cover every member or
// carry `otherwise` (enums.md D4). The message names the missing members, so a
// member added later points at each match that has not caught up.
type
    enum Light [off dim bright]
    class C
        public
            verb main(player is obj) returns int
                var level is Light = Light.dim
                match level
                    when off then player.tell("off")
                endmatch
                return 0
            endverb
    endclass
