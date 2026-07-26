// exs_decimal_lit.exs : decimal constants (numbers.md).
//
// decimal is base-10 fixed-point (value x 10^-4 in an int32), so any
// literal with at most four fraction digits is exact by construction:
// no 0f prefix, no fixed(num, den), no dyadic reasoning. A literal
// with nonzero digits past the fourth is a compile error, and float
// is the escape for finer values.

type
    class Tuning
        public
            // integer part adds with `+`; exact: 4.1201
            tunable weight     is decimal = 4 + 0.1201
            // the same value as a single literal
            tunable weight_lit is decimal = 4.1201
            // whole and half values are exact too
            tunable balance    is decimal = 0.5

            verb retune(target is obj)
                // decimal constants are ordinary values in expressions
                var half is decimal = balance
                target.apply(half)
                // decimal atoms in a data literal (a stat table)
                target.load([weights 0.75 0.125 0.9375])
            endverb
    endclass
