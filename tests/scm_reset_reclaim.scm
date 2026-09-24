; scm_reset_reclaim.scm : reset reclaims its continuation arena (exit 0 = pass)
;
; Each (reset (deep 60) handler) captures a continuation at the bottom of a
; 60-frame chain, so its buffer in the fixed 64 KB __cont_arena is large.
; Without stack-discipline reclamation the arena bump pointer only ever
; advances, so a few hundred turns run it off the end of the arena and on
; into adjacent memory, crashing.  With reclamation the arena is restored to
; its mark-time high water mark when each reset extent closes, so the arena
; never holds more than one turn's buffer and the loop runs cleanly.
;
; The turn count (300) is kept well under the ~1024 that would exhaust the
; separate 8 KB closure heap, so this test exercises the continuation arena
; in isolation.

(define (deep n)
  (if (= n 0)
      (shift)
      (+ 1 (deep (- n 1)))))

(define (handler k)
  (resume k 42))

(define (loop i)
  (if (= i 0)
      0
      (if (= (reset (deep 60) handler) 102)   ; 60 + 42
          (loop (- i 1))
          1)))                                ; a corrupted result would fail

(loop 300)
