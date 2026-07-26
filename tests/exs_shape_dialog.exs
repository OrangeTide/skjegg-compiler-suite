// exs_shape_dialog.exs : typed data literals (typed-data.md, the R5 pass).
// A `shape` schema checks a symbolic dialog tree: head words, slot arity
// and scalar types, and intra-tree `to` targets. A well-formed dialog
// passes the type checker. Shapes are static-only (D4): the literal does
// not lower, so this is a checker sample (skj-exc <file>), not run
// end-to-end.
// Made by a machine. PUBLIC DOMAIN (CC0-1.0)

type
    shape dialog
        dialog [node+]                  // one or more labelled nodes
        node   [label line+]            // a label, then one or more lines
        line   [text or choice]         // a line is spoken text or a choice
        text   [str]                    // a line of text
        choice [str to ref]             // a prompt, then a jump target
    endshape

    class Story
        public
            // a shape-typed parameter drives the check on the argument
            verb play(script is dialog) returns int
                return 1
            endverb

            verb main() returns int
                return self.play(
                    [dialog
                        [node greeting
                            [text "Hello traveler"]
                            [choice "About the barrow" to barrow]
                            [choice "Goodbye" to farewell]]
                        [node barrow
                            [text "The barrow is old and cold"]
                            [choice "Back" to greeting]]
                        [node farewell
                            [text "Safe travels"]]])
            endverb
    endclass
