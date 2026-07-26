// exs_utf8_r7.exs : text-encoding.md (R7). The four character operations
// count code points, not bytes. "café" is 4 code points but 5 UTF-8 bytes
// (é is 0xC3 0xA9). len counts code points, bytes counts raw bytes, s[i]
// and s[lo to hi] index/slice by code point, and `for c in s` yields whole
// code points (so c = "é" works). A list's len is its element count and
// bytes its raw word-sized storage.
type
    class C
        public
            verb main(player is obj) returns int
                var s is str = "café"
                player.tell("len ${length(s)}")            // 4 code points
                player.tell("bytes ${bytes(s)}")        // 5 bytes
                player.tell("at4 ${s[4]}")              // é
                player.tell("slice ${s[1 to 3]}")       // caf
                player.tell("tail ${s[2 to 4]}")        // afé
                var oor is str = s[5] otherwise "OOR"    // 5 > 4 code points,
                player.tell("oor ${oor}")               // even though byte-len is 5
                for c in s do
                    player.tell("c ${c}")               // c / a / f / é
                endfor
                var hit is bool = false
                for c in s do
                    if c = "é" then
                        hit = true
                    endif
                endfor
                player.tell("hit ${hit}")               // true
                var xs is list<int> = [10 20 30]
                player.tell("listlen ${length(xs)}")       // 3 elements
                player.tell("listbytes ${bytes(xs)}")   // 12 bytes (3 * 4)
                return 0
            endverb
    endclass
