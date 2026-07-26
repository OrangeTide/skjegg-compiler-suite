type
    class Sensor
        private
            reading is int = 42
        public
            verb read() returns int
                return reading
            endverb
            verb bump(by is int)
                reading = reading + by
            endverb
    endclass
    class Probe
        public
            verb main() returns int
                var s is Sensor = spawn(Sensor)
                s.bump(5)
                return s.read()
            endverb
    endclass
