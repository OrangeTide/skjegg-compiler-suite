// exs_err_iface_missing.exs : a class naming an interface it does not
// satisfy is an error AT THE CLASS (interface-support.md D3), which is the
// half of the feature that pays even when signatures are written out.
type
    interface Openable
        verb open() returns str
        verb close() returns str
    endinterface
    class Chest
        supports Openable
        public
            verb open() returns str
                return "creak"
            endverb
    endclass
