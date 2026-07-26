type
    class L
        public
            verb main() returns int
                var i is int = 0
                var acc is int = 0
                while true do
                    i = i + 1
                    if i > 10 then
                        break
                    elseif i = 5 then
                        continue
                    endif
                    acc = acc + i
                endwhile
                return acc
            endverb
    endclass
