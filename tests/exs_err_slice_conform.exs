// exs_err_slice_conform.exs : conformance is by verb set (object-slices.md
// D3). Rock has `open` but not `close`, so it does not satisfy Openable, and
// the mismatch is caught where the value crosses into the slice-typed
// parameter rather than at the send inside.
type
    interface Openable
        verb open() returns str
        verb close() returns str
    endinterface
    class Rock
        public
            verb open() returns str
                return "no"
            endverb
    endclass
    class C
        private
            func cycle(d is Openable) returns str
                return d.open()
            endfunc
        public
            verb main(player is obj) returns int
                var r is Rock = spawn(Rock)
                player.tell(cycle(r))
                return 0
            endverb
    endclass
