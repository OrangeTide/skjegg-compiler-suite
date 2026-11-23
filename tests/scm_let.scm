; let / let* / cond / when / unless / and / or / not
(display (let ((x 3) (y 4)) (+ (* x x) (* y y)))) (newline)
(display (let* ((a 2) (b (* a a)) (c (* b b))) c)) (newline)
(display (cond ((< 5 1) 100) ((= 2 2) 200) (else 300))) (newline)
(display (and 1 2 3)) (newline)
(display (and 1 #f 3)) (newline)
(display (or #f #f 7)) (newline)
(display (not #f)) (newline)
(display (not 5)) (newline)
(when (< 1 2) (display "when-yes") (newline))
(unless (< 2 1) (display "unless-yes") (newline))
(display (let ((n 10)) ((lambda (x) (+ x n)) 5))) (newline)
0
