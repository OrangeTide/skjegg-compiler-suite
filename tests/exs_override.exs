type
    class Animal
        public
            verb sound() returns int
                return 7
            endverb
    endclass
    class Dog is Animal
        public
            verb sound() returns int
                return 42
            endverb
            verb main() returns int
                return self.sound()
            endverb
    endclass
