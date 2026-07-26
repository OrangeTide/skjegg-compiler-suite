// exs_send_str.exs : str and fixed through sends (output.md). Both are
// one-word values, so they ride the flat argv of __exc_send unchanged:
// a checked send on a spawned class-typed receiver passes a str and a
// fixed argument and returns each type back.
// Made by a machine. PUBLIC DOMAIN (CC0-1.0)

type
    class Speaker
        public
            greeting is str = "hail"

            verb compose(who is str) returns str
                return self.greeting + ", " + who
            endverb

            verb rate(x is decimal) returns decimal
                return x * 3
            endverb
    endclass

    class C
        public
            verb main() returns int
                var s = spawn(Speaker)
                var r is int = 0

                var msg is str = s.compose("traveler")
                if msg = "hail, traveler" then
                    r = r + 1
                endif
                if length(msg) = 14 then
                    r = r + 2
                endif

                var f is decimal = s.rate(1.5)
                if f = 4.5 then
                    r = r + 4
                endif

                return r
            endverb
    endclass
