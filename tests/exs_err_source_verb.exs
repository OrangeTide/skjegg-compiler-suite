// a source is second-class (sources.md D5): it never crosses the actor
// boundary, so a verb cannot take one
type
    class C
        public
            verb feed(s is source of int)
            endverb
            verb main()
            endverb
    endclass
