// exs_err_slice_verb.exs : a slice permits exactly the verbs it names
// (object-slices.md D5), even when the runtime object would understand more.
// That restriction is what makes a slice an attenuated capability: handing
// out `chest with (open, close)` hands over two verbs and nothing else.
type
    interface Openable
        verb open() returns str
        verb close() returns str
    endinterface

    class Chest
        private
            shut is bool = true
        public
            verb open() returns str
                self.shut = false
                return "the chest creaks open"
            endverb
            verb close() returns str
                self.shut = true
                return "the chest thuds shut"
            endverb
            verb smash() returns str
                return "splinters"
            endverb
    endclass

    class Door
        public
            verb open() returns str
                return "the door swings wide"
            endverb
            verb close() returns str
                return "the door clicks"
            endverb
    endclass

    class C
        private
            // any object that opens and closes, whatever its class
            func cycle(d is Openable) returns str
                return d.open() + d.smash()
            endfunc
        public
            verb main(player is obj) returns int
                var c is Chest = spawn(Chest)
                var w is Door = spawn(Door)
                player.tell(cycle(c))
                player.tell(cycle(w))
                return 0
            endverb
    endclass
