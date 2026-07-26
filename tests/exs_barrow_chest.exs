// barrow_chest.exs
use
    world
    items as it

type
    enum Element [fire frost poison arcane]

    class BarrowChest is Container
        private
            locked       is bool = true
            opened_count is int  = 0

            func reward(opener is obj)
                var prize = spawn(it.Sunstone)
                opener.receive(prize)
                opener.grant_gold(self.gold_reward)
            endfunc
        public
            tunable gold_reward is int = 50

            verb on_open(opener is obj)
                if self.locked then
                    opener.tell("The barrow chest is locked fast.")
                    return
                endif
                self.opened_count = self.opened_count + 1
                reward(opener)
            endverb

            verb unlock(key is obj) returns bool
                if not (key is it.BarrowKey) then
                    return false
                endif
                self.locked = false
                return true
            endverb
    endclass
