// exs_err_shape_misplaced.exs : a node kind that is real but in the wrong
// place reads differently from a misspelled one, so it gets its own message
// (typed-data.md): the kind exists, it is not allowed here.
type
    shape dialog
        dialog [node+]
        node   [label line+]
        line   [text]
        text   [str]
    endshape

    class Story
        public
            verb play(script is dialog) returns int
                return 1
            endverb
            verb main() returns int
                return self.play(
                    [dialog
                        [node greeting
                            [node inner
                                [text "nested wrongly"]]]])
            endverb
    endclass
