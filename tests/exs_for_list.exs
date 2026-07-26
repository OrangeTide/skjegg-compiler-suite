type
    class C
        public
            verb main(player is obj) returns int
                var xs is list<int> = [10 20 30 40]
                var sum is int = 0
                for v in xs do
                    sum = sum + v
                endfor
                player.tell("${sum}")

                var names is list<str> = ["red" "green" "blue"]
                var joined is str = ""
                for c in names do
                    joined = joined + c + " "
                endfor
                player.tell("${joined}")

                var built is list<int> = []
                for i in 1 to 5 do
                    built = append(built, i * i)
                endfor
                var total is int = 0
                for v in built do
                    if v = 9 then
                        continue
                    endif
                    if v = 25 then
                        break
                    endif
                    total = total + v
                endfor
                player.tell("${total}")

                var count is int = 0
                for ch in "hello" do
                    count = count + 1
                endfor
                player.tell("${count}")
                return 0
            endverb
    endclass
