REM REMOVE (swap with last) and CLEAR
DIM v AS VECTOR OF INT
DIM i AS INT
FOR i = 0 TO 4
  APPEND v, i
NEXT i
REM v = 0 1 2 3 4; remove index 1 moves last (4) into slot 1
REMOVE v, 1
PRINT "after remove: count="; COUNT(v)
PRINT "v="; v(0); " "; v(1); " "; v(2); " "; v(3)

REM remove the last element (no move)
REMOVE v, COUNT(v) - 1
PRINT "count="; COUNT(v)

REM CLEAR empties it
CLEAR v
PRINT "cleared="; COUNT(v)

REM reuse after clear
APPEND v, 42
PRINT "reuse="; COUNT(v); " "; v(0)

REM REMOVE on a STRING vector
DIM s AS VECTOR OF STRING
APPEND s, "a"
APPEND s, "b"
APPEND s, "c"
REMOVE s, 0
PRINT "s="; s(0); s(1); " "; COUNT(s)
END
