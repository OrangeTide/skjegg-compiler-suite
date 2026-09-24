; scm_reset_heap.scm : a global function used as a value is a static closure
;                      (exit 0 = pass)
;
; (reset (produce) handler) names `handler`, a global function, as a value.
; If each turn boxed a fresh { tag, code-ptr } closure from the 8 KB __heap,
; the loop would exhaust it after about 1024 turns and crash.  With one
; static closure per function the reference is a constant address and the
; loop allocates nothing per turn, so 3000 turns (well past the old limit)
; run cleanly.  Paired with the continuation-arena reclamation at reset,
; this makes a repeatedly-capturing program bounded in both arenas.

(define (produce)
  (+ (shift) 100))

(define (handler k)
  (resume k 42))

(define (loop n)
  (if (= n 0)
      0
      (if (= (reset (produce) handler) 142)
          (loop (- n 1))
          1)))

(loop 3000)
