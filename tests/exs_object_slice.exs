// exs_object_slice.exs : an object slice is a structural verb subset, the
// interface type (object-slices.md). `Openable is obj with (open, close)`
// accepts any class that responds to those verbs, with no class declaring
// conformance: Chest and Door share no parent and neither mentions Openable.
//
// A slice value at rest is the plain object handle, so the parameter costs
// nothing, and the send goes through the ordinary dispatch. The verb
// signature comes from the selector, which is program-global, so `d.open()`
// is typed str here and concatenates rather than being an untyped `any`.
//
// A slice narrows to a smaller slice: `Openable` passes to a `Shuttable`
// parameter, since its verb set contains the smaller one.
type
    interface Openable
        verb open() returns str
        verb close() returns str
    endinterface

    interface Shuttable
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
            func shut(d is Shuttable) returns str
                return d.close()
            endfunc
            func cycle(d is Openable) returns str
                return d.open() + " / " + shut(d)
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
