// exs_param_default.exs : function parameter defaults (compile-time constant),
// usable positionally-omitted or named-omitted.
type
    class C
        private
            func greet(base is int, bonus is int = 10) returns int
                return base + bonus
            endfunc
        public
            verb main(player is obj) returns int
                player.tell("all ${greet(5, 20)}")        // 25
                player.tell("posdef ${greet(5)}")          // 15 (bonus defaults)
                player.tell("named ${greet(base: 7)}")     // 17 (bonus defaults)
                player.tell("both ${greet(bonus: 1, base: 2)}") // 3
                return 0
            endverb
    endclass
