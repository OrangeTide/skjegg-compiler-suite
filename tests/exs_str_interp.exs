type
    class C
        private
            name is str = "gandalf"
        public
            verb main(player is obj) returns int
                var n is int = 42
                var f is float = 3.5
                var x is decimal = 3.5
                player.tell("hi ${name}!")
                player.tell("n=${n} f=${f} x=${x}")
                player.tell("sum ${n + 8}")
                player.tell("greet ${"dear " + name}")
                player.tell("write \${n} to splice; $5 needs no escape")
                return 0
            endverb
    endclass
