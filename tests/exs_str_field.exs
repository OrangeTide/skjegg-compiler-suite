type
    class C
        private
            name is str = "anon"
        public
            verb rename(who is str)
                name = who
            endverb
            verb main(player is obj) returns int
                player.tell("${name}")
                rename("gandalf")
                player.tell("${self.name}")
                self.name = name + "!"
                player.tell("${name}")
                if name = "gandalf!" then
                    return 7
                endif
                return 0
            endverb
    endclass
