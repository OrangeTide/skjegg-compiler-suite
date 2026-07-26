// exs_err_iface_mismatch.exs : a class that writes a signature disagreeing
// with the interface it supports (interface-support.md D3).
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
            verb open() returns int
                self.shut = false
                return 1
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
