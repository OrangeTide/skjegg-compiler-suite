// exs_list_fixed.exs : data literals carry their element type. A
// homogeneous constant literal is a list<T> (checked in the type
// checker); fixed elements lower as scaled words; an unascribed
// literal infers its list type.
type
    class C
        public
            verb main(player is obj) returns int
                var xs is list<decimal> = [1.5 2.5]
                var inferred = [10 20 30]      // infers list<int>
                for v in xs do
                    player.tell("${v}")                     // 1.500000 / 2.500000
                endfor
                return length(inferred)           // 3
            endverb
    endclass
