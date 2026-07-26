// exs_div.exs : divide and modulo are fallible producers
// (runtime-errors.md). A zero divisor fails Icon-style: `otherwise`
// supplies the default, `if var` branches, and a fallible func propagates.
// Made by a machine. PUBLIC DOMAIN (CC0-1.0)

type
    class C
        private
            func ratio(a is int, b is int) returns maybe int
                return a / b            // /0 propagates as the failure
            endfunc
        public
            verb main() returns int
                var a is int = 7
                var z is int = 0
                var two is int = 2          // nonzero variable divisor
                var dz is decimal = 0.0     // zero variable divisor
                var r is int = 0

                if a / 2 = 3 then            // constant divisor: infallible then
                    r = r + 1
                endif
                if a % 2 = 1 then
                    r = r + 2
                endif

                var d is int = a / z otherwise 99
                if d = 99 then
                    r = r + 4
                endif
                var m is int = a % z otherwise 7
                if m = 7 then
                    r = r + 8
                endif

                if var q = a / z then
                    r = r + q           // must not run
                else
                    r = r + 16
                endif
                if var q2 = a / two then     // variable divisor: fallible, binds 3 then
                    r = r + q2          // 3
                endif

                r = r + (ratio(12, 4) otherwise 100)    // 3
                r = r + (ratio(12, 0) otherwise 100)    // 100: propagated

                var f is decimal = 6.0 / 2.0
                if f = 3.0 then
                    r = r + 32
                endif
                var g is decimal = 1.0 / dz otherwise 9.0  // variable /0: fails
                if g = 9.0 then
                    r = r + 64
                endif

                return r
            endverb
    endclass
