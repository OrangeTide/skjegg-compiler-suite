; display / newline / write-string: numbers, booleans, nil, string literals
(write-string "6! = ")
(define (fact n) (if (= n 0) 1 (* n (fact (- n 1)))))
(display (fact 6))
(newline)
(display -42)
(newline)
(display 0)
(newline)
(display (< 1 2))
(newline)
(display (< 2 1))
(newline)
0
