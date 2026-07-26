// exs_err_nosig_nosource.exs : a verb written with no signature needs a
// source for one (interface-support.md D4/D5): a supported interface or the
// parent. With neither, the omission is the error.
type
    class Chest
        public
            verb open
                return 1
            endverb
    endclass
