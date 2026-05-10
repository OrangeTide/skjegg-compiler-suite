; scm_pairs.scm : test pair primitives — exit 0 = all pass

(define (test_cons)
  (+ (- (car (cons 3 7)) 3)
     (- (cdr (cons 3 7)) 7)))

(define (test_null)
  (+ (if (null? ()) 0 1)
     (if (null? (cons 1 2)) 1 0)))

(define (test_pair)
  (+ (if (pair? (cons 1 2)) 0 1)
     (if (pair? ()) 1 0)))

(define (test_mutate p)
  (begin
    (set-car! p 10)
    (set-cdr! p 20)
    (+ (- (car p) 10)
       (- (cdr p) 20))))

(define (test_list)
  (+ (- (car (list 5 6 7)) 5)
     (+ (- (car (cdr (list 5 6 7))) 6)
        (if (null? (cdr (cdr (cdr (list 5 6 7))))) 0 1))))

(+ (+ (+ (+ (test_cons) (test_null)) (test_pair)) (test_mutate (cons 0 0))) (test_list))
