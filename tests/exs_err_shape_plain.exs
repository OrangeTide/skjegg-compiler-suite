// exs_err_shape_plain.exs : a shape-typed context needs a data node headed by
// the shape's root kind, not an empty or plain list literal (typed-data.md).
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
                return self.play([])
            endverb
    endclass
