// exs_err_iface_peers.exs : two supported interfaces declaring one verb
// differently is the peers-error case (interface-support.md D5, reusing
// capabilities.md D4). The class resolves it by writing the signature.
type
    interface A
        verb ping() returns int
    endinterface
    interface B
        verb ping() returns str
    endinterface
    class Chest
        supports A, B
        public
            verb ping
                return 1
            endverb
    endclass
