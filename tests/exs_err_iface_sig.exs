// exs_err_iface_sig.exs : a declared interface states its signatures, so
// conformance compares them (interface-decl.md D6). Chest has both verbs by
// name, but `open` returns int where Openable says str, so it does not
// satisfy the contract and the mismatch is caught where the value crosses
// into the interface-typed parameter.
type
    interface Openable
        verb open() returns str
        verb close() returns str
    endinterface
    class Chest
        public
            verb open() returns int
                return 1
            endverb
            verb close() returns str
                return "thud"
            endverb
    endclass
    class C
        private
            func cycle(d is Openable) returns str
                return d.open()
            endfunc
        public
            verb main(player is obj) returns int
                var c is Chest = spawn(Chest)
                player.tell(cycle(c))
                return 0
            endverb
    endclass
