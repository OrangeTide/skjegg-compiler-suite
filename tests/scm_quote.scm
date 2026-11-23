; quoted data: numbers, booleans, nil, lists, and symbol identity via eq?
(define (sum lst)
  (if (null? lst)
      0
      (+ (car lst) (sum (cdr lst)))))

(display (sum '(1 2 3 4 5))) (newline)     ; 15
(display (car '(10 20 30))) (newline)       ; 10
(display (car (cdr '(10 20 30)))) (newline) ; 20

(display (if (null? '()) 1 0)) (newline)    ; 1 (quoted empty list is nil)
(display (if (pair? '(9)) 1 0)) (newline)   ; 1

; symbol identity: same name interns to one object, different names differ
(display (if (eq? 'apple 'apple) 1 0)) (newline)   ; 1
(display (if (eq? 'apple 'banana) 1 0)) (newline)   ; 0

; eq? on immediates
(display (if (eq? 7 7) 1 0)) (newline)              ; 1
(display (if (eq? '() '()) 1 0)) (newline)          ; 1
