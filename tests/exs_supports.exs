// exs_supports.exs : a class names the interfaces it supports
// (interface-support.md). `supports` is a class-body directive like `include`,
// it is optional (structural conformance is unchanged), and it brings
// signatures and no members.
//
// The verbs are written with no signature at all: `verb open`, not
// `verb open() returns str`. The signature comes from the interface, and so do
// the parameter names, which is why `hurt` can use `amount` without declaring
// it. Abbreviation is all-or-nothing, so `verb open()` still means "takes
// nothing".
type
    interface Openable
        verb open() returns str
        verb close() returns str
    endinterface
    interface Damageable
        verb hurt(amount is int, kind is str)
    endinterface

    class Chest
        supports Openable, Damageable
        private
            shut is bool = true
            hp is int = 20
        public
            verb open
                self.shut = false
                return "it creaks open"
            endverb
            verb close
                self.shut = true
                return "it thuds shut"
            endverb
            verb hurt
                self.hp = self.hp - amount
            endverb
            verb hpleft() returns int
                return self.hp
            endverb
    endclass

    class C
        private
            func cycle(d is Openable) returns str
                return d.open() + " / " + d.close()
            endfunc
        public
            verb main(player is obj) returns int
                var c is Chest = spawn(Chest)
                player.tell(cycle(c))
                c.hurt(5, "fire")
                player.tell("hp ${c.hpleft()}")
                return 0
            endverb
    endclass
