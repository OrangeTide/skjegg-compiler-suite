// exs_tell.exs : the output capability (output.md). World text is a
// message to a reader: the host hands main(player is obj) the console
// object, and player.tell(msg) is an ordinary send whose str argument
// rides the flat argv. Formatting is ${} interpolation at the sender.
// trace and /// are the author channel: compiled only under -t, so
// they are no-ops here and stdout matches .expected exactly.
// Made by a machine. PUBLIC DOMAIN (CC0-1.0)

type
    class C
        private
            func greet(who is str) returns str
                return "hello, " + who
            endfunc
            func avg(a is float, b is float) returns float
                return (a + b) / 2.0
            endfunc
            func scaled(x is decimal, k is int) returns decimal
                return x * k
            endfunc
        public
            verb main(player is obj) returns int
                player.tell("start")
                trace "invisible without -t"
                /// also invisible without -t
                player.tell(greet("world"))
                player.tell("${42}")
                player.tell("${-7}")
                player.tell("${avg(10.0, 20.0)}")
                var half is decimal = 0.5
                player.tell("${half}")
                player.tell("${scaled(half, 5)}")
                return 0
            endverb
    endclass
