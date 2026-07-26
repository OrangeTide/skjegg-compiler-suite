// exs_err_shape_ref.exs : a `to` target that names no label in the tree is caught at compile time

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
                            [choice "Goodbye" to nowhere]]
                        [node barrow
                            [text "The barrow is old and cold"]
                            [choice "Back" to greeting]]
                        [node farewell
                            [text "Safe travels"]]])
            endverb
    endclass
