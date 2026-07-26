// exs_set_of.exs : `set of E`, a flag bitmask over an enum universe
// (set-of.md). A set is a compile-time-sized bitmask value (a single word for
// a small enum, D1), reusing int storage for vars, fields, params, and sends.
// Members are bare singleton sets in a set-pinned context (D4); value-building
// is symbols (`+` union, `-` difference, `^` toggle, D5) with `empty`/`full`
// identities; the two tests are words (`in` subset, `overlaps` intersection,
// D6). `set` and `empty`/`full` stay ordinary names (the `set` list builtin, a
// variable named `empty`); the set forms are contextual.
type
    enum Element [fire ice poison acid]

    class Mob
        private
            resist is set of Element = empty        // a set field, defaults empty
        public
            verb ward(v is set of Element)          // a set parameter
                resist = v
            endverb
            verb resists(e is Element) returns bool
                return e in resist                  // a runtime member test
            endverb
    endclass

    class C
        public
            verb main(player is obj) returns int
                var r is set of Element = fire + poison    // bare members (D4)
                player.tell("mem ${fire in r} ${ice in r}")        // true false

                var more is set of Element = r + Element.ice       // qualified also ok
                player.tell("ov ${more overlaps r} sub ${r in more}")   // true true

                var gone is set of Element = more - fire           // difference
                player.tell("diff ${fire in gone} ${ice in gone}")      // false true

                var tog is set of Element = r ^ fire               // toggle fire off
                player.tell("tog ${fire in tog} ${poison in tog}")      // false true

                var none is set of Element = empty
                var all is set of Element = full
                player.tell("id ${fire in none} ${acid in all}")        // false true

                var m is Mob = spawn(Mob)
                var w is set of Element = fire + ice
                m.ward(w)                                          // a set through a send
                player.tell("ward ${m.resists(Element.fire)} ${m.resists(Element.poison)}")  // true false
                return 0
            endverb
    endclass
