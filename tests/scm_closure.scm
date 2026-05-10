; scm_closure.scm : test closures with captured variables — exit 0 = all pass

(define (make_adder n)
  (lambda (x) (+ x n)))

(define add5 (make_adder 5))
(define add10 (make_adder 10))

(define (test_basic)
  (+ (- (add5 3) 8)
     (- (add10 3) 13)))

(define (make_linear a b)
  (lambda (x) (+ (* a x) b)))

(define f (make_linear 2 3))

(define (test_multi_capture)
  (- (f 5) 13))

(define (apply_fn g x)
  (g x))

(define (test_pass_closure)
  (- (apply_fn (make_adder 7) 3) 10))

(define (double x) (* x 2))

(define (test_named_as_value)
  (- (apply_fn double 3) 6))

(+ (+ (+ (test_basic) (test_multi_capture)) (test_pass_closure)) (test_named_as_value))
