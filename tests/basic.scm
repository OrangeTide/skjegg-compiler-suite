; basic.scm : TinScheme parser test

(define (fact n)
  (if (= n 0)
      1
      (* n (fact (- n 1)))))

(define (add-one x) (+ x 1))

(define (map f lst)
  (if (null? lst)
      '()
      (cons (f (car lst))
            (map f (cdr lst)))))

(define pi 314)

(define greeting "hello, tinscheme")

(define truth #t)
(define falsehood #f)

; dotted pair
(define pair '(1 . 2))

; nested quoted list
(define data '(1 (2 3) 4))

; higher-order: pass lambda to map
(map (lambda (x) (+ x 10)) '(1 2 3))
