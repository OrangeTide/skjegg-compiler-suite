; scm_fact.scm : factorial — exit code 0 means (fact 5) == 120
(define (fact n)
  (if (= n 0)
      1
      (* n (fact (- n 1)))))
(- (fact 5) 120)
