// exs_err_iface_bare.exs : the bare-identifier declaration retired
// (interface-decl.md D7). Nothing in a type section opens with a name now,
// so the message names the declaration keywords: this is either a misspelled
// one or the old interface form, and guessing between them is what the
// keyword form exists to avoid.
type
    Openable is obj with (open, close)

    class C
        public
            verb main(player is obj) returns int
                return 0
            endverb
    endclass
