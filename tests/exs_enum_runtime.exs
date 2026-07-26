// exs_enum_runtime.exs : the enum runtime (enums.md). A member is its 0-based
// ordinal, a small int (D1), written qualified in value positions
// (`Light.dim`, D2) and bare as a match label (D3). Equality is within one
// enum (D5), a match must be exhaustive or carry `otherwise` (D4), and a hole
// prints the member name via the per-enum table (D6). Enums flow through
// fields, params, returns, and sends like any word.
type
    enum Light [off dim bright]

    class Lamp
        private
            level is Light = Light.off          // an enum field default
        public
            verb set(v is Light)                // an enum parameter
                level = v
            endverb
            verb show(player is obj)
                player.tell("level ${level}")   // prints the member name (D6)
            endverb
            verb brighter() returns Light        // an enum return
                match level                      // bare member labels (D3)
                    when off then return Light.dim
                    when dim then return Light.bright
                    when bright then return Light.bright
                endmatch
            endverb
    endclass

    class C
        public
            verb main(player is obj) returns int
                var a is Light = Light.dim
                var b is Light = Light.dim
                player.tell("eq ${a = b} ne ${a = Light.off}")   // true false
                player.tell("name ${a}")                          // dim

                match a                                           // value list + otherwise
                    when off then player.tell("O")
                    when dim, bright then player.tell("on")
                endmatch

                var lamp is Lamp = spawn(Lamp)
                lamp.show(player)                                 // off
                lamp.set(Light.bright)
                lamp.show(player)                                 // bright
                player.tell("next ${lamp.brighter()}")            // bright
                return 0
            endverb
    endclass
