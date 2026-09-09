// exs_err_obj_default.exs : an object field defaults to nil; a non-nil
// default names an actor that does not exist until spawn (the field holds a
// handle, and the class image is a compile-time constant).
type
    class Room
        public
            verb name() returns str
                return "hall"
            endverb
    endclass
    class Hero
        private
            home is Room = spawn(Room)
        public
            verb probe() returns int
                return 1
            endverb
    endclass
    class C
        public
            verb main(player is obj) returns int
                return 0
            endverb
    endclass
