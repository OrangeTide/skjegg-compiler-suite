; scm_tailcall.scm : tail-recursive factorial — exit code 0 means success
(define (fact_iter n acc)
  (if (= n 0)
      acc
      (fact_iter (- n 1) (* n acc))))
(- (fact_iter 5 1) 120)
