; scm_shift.scm : delimited continuation test (exit 0 = pass)
;
; (reset body handler) evaluates body with a delimiter.  If (shift)
; fires inside body, the handler is called with the captured
; continuation buffer.  (resume buf val) resumes the continuation.

(define (produce)
  (+ (shift) 100))

(define (handler k)
  (resume k 42))

(- (reset (produce) handler) 142)
