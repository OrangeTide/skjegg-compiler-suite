; numeric primitives: quotient, remainder, modulo, abs, min, max
(display (quotient 17 5)) (newline)      ; 3
(display (remainder 17 5)) (newline)     ; 2
(display (remainder -17 5)) (newline)    ; -2 (truncates toward zero)
(display (modulo 17 5)) (newline)        ; 2
(display (modulo -17 5)) (newline)       ; 3 (floors, takes divisor sign)
(display (modulo 17 -5)) (newline)       ; -3
(display (modulo -17 -5)) (newline)      ; -2
(display (abs -42)) (newline)            ; 42
(display (abs 42)) (newline)             ; 42
(display (min 3 8)) (newline)            ; 3
(display (max 3 8)) (newline)            ; 8
(display (min -4 -9)) (newline)          ; -9
(display (max -4 -9)) (newline)          ; -4
